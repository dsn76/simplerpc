
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <tlsf_txn.h>

#include "libsrpc_local.h"
#include "libsrpc_errno.h"
#include "libsrpc_shmem.h"
#include "libsrpc_shm_gc.h"


int libsrpc_shmem_create(libsrpc_shmem_pool_t *pool, char *name, size_t size, uintptr_t virtaddr)
{
    int rc = 0;

    if(!pool || !name) return(-EINVAL);

    //pool->shm_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0666);
    pool->shm_fd = memfd_create(name, MFD_CLOEXEC);

    if (pool->shm_fd < 0) {
        rc = -errno;
        ERR_PRINT("shm_open failed: %s\n", strerror(errno));
        goto err;
    }

    if (ftruncate(pool->shm_fd, size) < 0) {
        rc = -errno;
        ERR_PRINT("ftruncate failed: %s\n", strerror(errno));
        goto err;
    }

    pool->shm_size = size;

    pool->shm = mmap( (void*)virtaddr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, pool->shm_fd, 0);
    if (pool->shm == MAP_FAILED) {
        pool->shm = NULL;
        rc = -errno;
        ERR_PRINT("mmap failed: %s\n", strerror(errno));
        goto err;
    }

    // Запрещаем наследование при fork
    rc = madvise(pool->shm, size, MADV_DONTFORK);
    if (rc != 0) {
        ERR_PRINT("madvise failed: %s\n", strerror(errno));
       goto err;
    }
DBG_PRINT("INIT shm\n");
    memset(pool->shm, 0, size); // инициализируем память нулями
    pool->shm->basevadr = virtaddr;
    pool->shm->shmsize = size;
    strncpy(pool->shm->sign, name, sizeof(pool->shm->sign)-1);

DBG_PRINT("INIT mempool in shm\n");
    pool->shm->poolsize = pool->shm->shmsize - sizeof(*pool->shm);
    DBG_PRINT("INIT mempool in shm size=%zu\n", pool->shm->poolsize);
    tlsf_t tlsf = tlsf_create(pool->shm->poolptr, pool->shm->poolsize);
    DBG_PRINT("tlsf_create =%d\n",tlsf_get_errno(tlsf));
    if(!tlsf) {
        rc = -ENOMEM;
        ERR_PRINT("tlsf_create failed\n");
        goto err;
    }

DBG_PRINT("INIT proc list\n");
    rc = libsrpc_list_head_init(&pool->shm->proc_head);
    if(rc < 0) {
        ERR_PRINT("libsrpc_list_head_init failed rc=%d\n", rc);
        goto err;
    }

    libsrpc_shm_gc_init();

end:
    return(rc);
err:
    if(pool->shm) munmap(pool->shm, size);
    if(pool->shm_fd > 0) close(pool->shm_fd);
    goto end;
}

int libsrpc_shmem_open(libsrpc_shmem_pool_t *pool, int shm_fd, uintptr_t virtaddr, const char *name)
{
    int rc = 0;
    size_t size = 0;
    struct stat st = {0};
    void *shm = NULL;

    if(fstat(shm_fd, &st) < 0) {
        rc = -errno;
        ERR_PRINT("libsrpc_shmem_open: fstat failed: %s\n", strerror(errno));
        goto err;
    }

    size = st.st_size;

    shm = mmap( (void*)virtaddr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, shm_fd, 0);
    if (shm == MAP_FAILED) {
        shm = NULL;
        rc = -errno;
        ERR_PRINT("libsrpc_shmem_open: mmap failed: %s\n", strerror(errno));
        goto err;
    }

    /* Проверяем сигнатуру */
    if(strncmp(((libsrpc_shmem_t*)shm)->sign, name, sizeof(pool->shm->sign)) != 0) {
        rc = -EINVAL;
        ERR_PRINT("libsrpc_shmem_open: sign mismatch\n");
        goto err;
    }

    // Запрещаем наследование при fork
    rc = madvise(shm, size, MADV_DONTFORK);
    if (rc != 0) {
        ERR_PRINT("libsrpc_shmem_open: madvise failed: %s\n", strerror(errno));
       goto err;
    }

    pool->shm = shm;
    pool->shm_fd = shm_fd;
    pool->shm_size = size;

    return(rc);
err:
    if(shm) munmap(shm, size);
    return(rc);
}


int libsrpc_shmem_destroy(libsrpc_shmem_pool_t *pool)
{
    int rc = 0;

    libsrpc_shm_gc_destroy();

    rc = libsrpc_list_head_destroy(&pool->shm->proc_head);
    if(rc < 0) {
        ERR_PRINT("libsrpc_list_head_destroy failed rc=%d\n", rc);
    }

    tlsf_destroy((tlsf_t)pool->shm->poolptr);

    rc = munmap(pool->shm, pool->shm->shmsize);
    if(rc < 0) {
        ERR_PRINT("munmap failed rc=%d\n", rc);
    }

    close(pool->shm_fd);

    return(rc);
}

libsrpc_shmem_t * libsrpc_shmem_get(void)
{
    libsrpc_shmem_t *shm = NULL;

    if(!simplerpc_data || !simplerpc_data->shmempool.shm) return(NULL);
    shm = simplerpc_data->shmempool.shm;
    return shm;
}

/* ------------------------------------------------------------------------------ */
/* Выделение памяти в разделяемой памяти */
/* ------------------------------------------------------------------------------ */
void* libsrpc_shmem_malloc_type(size_t size, libsrpc_shm_data_type_t type)
{
    void *ptr = NULL;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    uint16_t uid;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    uid = simplerpc_data->proc_uid;

    if(!shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    ptr = tlsf_malloc((tlsf_t)shm->poolptr, size, uid, type);
    __libsrpc_errno_set(tlsf_get_errno((tlsf_t)shm->poolptr));

end:
    return(ptr);
}

void* libsrpc_shmem_malloc(size_t size)
{
    return libsrpc_shmem_malloc_type(size, LIBSRPC_SHMDT_USER);
}

void* libsrpc_shmem_calloc_type(size_t num, size_t size, libsrpc_shm_data_type_t type)
{
    void *ptr = NULL;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    uint16_t uid;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    uid = simplerpc_data->proc_uid;

    if(!shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    ptr= tlsf_calloc((tlsf_t)shm->poolptr, num, size, uid, type);
    __libsrpc_errno_set(tlsf_get_errno((tlsf_t)shm->poolptr));

end:
    return(ptr);
}

void* libsrpc_shmem_calloc(size_t num, size_t size)
{
    return libsrpc_shmem_calloc_type(num, size, LIBSRPC_SHMDT_USER);
}

void* libsrpc_shmem_realloc(void* ptr, size_t newsize)
{
    void *nptr = NULL;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    uint16_t uid;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    uid = simplerpc_data->proc_uid;

    if(!shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    nptr = tlsf_realloc((tlsf_t)shm->poolptr, ptr, newsize, uid);
    __libsrpc_errno_set(tlsf_get_errno((tlsf_t)shm->poolptr));

end:
    return(nptr);
}

void libsrpc_shmem_free(void *ptr)
{
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    uint16_t uid;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    uid = simplerpc_data->proc_uid;

    if(!shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    tlsf_free((tlsf_t)shm->poolptr, ptr, uid);
    __libsrpc_errno_set(tlsf_get_errno((tlsf_t)shm->poolptr));

end:
    return;
}

void  libsrpc_shmem_free_dc(void *ptr)
{
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    int rc = 0;

    if(!shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    rc = tlsf_setdc((tlsf_t)shm->poolptr, ptr);
    __libsrpc_errno_set(rc);

end:
    return;
}

int libsrpc_shmem_link(void *ptr)
{
    int rc;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    uint16_t uid;

    if(!ptr || !simplerpc_data || !shm) return(-EINVAL);
    uid = simplerpc_data->proc_uid;

    rc = tlsf_link((tlsf_t)shm->poolptr, ptr, uid);

    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
end:
    return(rc);
}


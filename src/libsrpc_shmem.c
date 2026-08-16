
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

#include "libsrpc_local.h"
#include "libsrpc_errno.h"
#include "libsrpc_shmem.h"
#include "libsrpc_mpmcq.h"


#define TLSF_BUL (1)
#include <tlsf.h>


static int robust_mutex_init(libsrpc_shmem_t *shm)
{
    pthread_mutex_t *mutex = &shm->mutex;

    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);

    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(mutex, &attr);

    pthread_mutexattr_destroy(&attr);

    return(0);
}

static int robust_mutex_destroy(libsrpc_shmem_t *shm)
{
    pthread_mutex_t *mutex = &shm->mutex;
    return pthread_mutex_destroy(mutex);
}

static int robust_mutex_lock(libsrpc_shmem_t *shm)
{
    pthread_mutex_t *mutex = &shm->mutex;
    int rc = pthread_mutex_lock(mutex);
    if (rc == EOWNERDEAD) {
        shm->cnt_errownerdead++;
        rc = tlsf_bul_recover(shm->poolptr);
        ERR_PRINT("Owner died. Recover mutex.\n");
        pthread_mutex_consistent(mutex);
    }
    return(rc);
}

static int robust_mutex_unlock(libsrpc_shmem_t *shm)
{
    pthread_mutex_t *mutex = &shm->mutex;
    return pthread_mutex_unlock(mutex);
}

int libsrpc_shmem_create(libsrpc_shmem_pool_t *pool, char *name, size_t size, uintptr_t virtaddr)
{
    int rc = 0;

    if(!pool || !name) return(-EINVAL);

    //pool->shm_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0666);
    pool->shm_fd = memfd_create(name, MFD_CLOEXEC);

    if (pool->shm_fd < 0) {
        rc = -errno;
        ERR_PRINT("libsrpc_shmem_create: shm_open failed: %s\n", strerror(errno));
        goto err;
    }

    if (ftruncate(pool->shm_fd, size) < 0) {
        rc = -errno;
        ERR_PRINT("libsrpc_shmem_create: ftruncate failed: %s\n", strerror(errno));
        goto err;
    }

    pool->shm_size = size;

    pool->shm = mmap( (void*)virtaddr, size, PROT_READ | PROT_WRITE, MAP_SHARED, pool->shm_fd, 0);
    if (pool->shm == MAP_FAILED) {
        pool->shm = NULL;
        rc = -errno;
        ERR_PRINT("libsrpc_shmem_create: mmap failed: %s\n", strerror(errno));
        goto err;
    }

    // Запрещаем наследование при fork
    rc = madvise(pool->shm, size, MADV_DONTFORK);
    if (rc != 0) {
        ERR_PRINT("libsrpc_shmem_create: madvise failed: %s\n", strerror(errno));
       goto err;
    }

    memset(pool->shm, 0, size); // инициализируем память нулями
    pool->shm->basevadr = virtaddr;
    pool->shm->shmsize = size;
    strncpy(pool->shm->sign, name, sizeof(pool->shm->sign)-1);

    pool->shm->poolsize = pool->shm->shmsize - sizeof(*pool->shm);
    init_memory_pool(pool->shm->poolsize, pool->shm->poolptr);

    robust_mutex_init(pool->shm);
    rc = list_init(&pool->shm->reg_pid_list, 0);
    DBG_PRINT("!!!! libsrpc_shmem_create: reg_pid_list init rc=%d\n", rc);
    if(rc < 0) {
        ERR_PRINT("libsrpc_shmem_create: list_init failed\n");
        goto err;
    }

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

    shm = mmap( (void*)virtaddr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
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

    robust_mutex_destroy(pool->shm);
    rc = list_destroy(&pool->shm->reg_pid_list);
    if(rc < 0) {
        ERR_PRINT("libsrpc_shmem_destroy: list_destroy failed\n");
    }

    destroy_memory_pool(pool->shm->poolptr);

    rc = munmap(pool->shm, pool->shm->shmsize);

    close(pool->shm_fd);

    return(rc);
}

void* libsrpc_shmem_malloc(size_t size)
{
    int rc = 0;
    void *ptr = NULL;
    libsrpc_shmem_t *shm;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    if(!simplerpc_data->shmempool.shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    shm = simplerpc_data->shmempool.shm;

    rc = robust_mutex_lock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }

    ptr = malloc_ex(size, shm->poolptr);

    rc = robust_mutex_unlock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
end:
    return(ptr);
}

void* libsrpc_shmem_calloc(size_t num, size_t size)
{
    void *ptr = NULL;
    int rc = 0;
    libsrpc_shmem_t *shm;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    if(!simplerpc_data->shmempool.shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    shm = simplerpc_data->shmempool.shm;

    rc = robust_mutex_lock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }

    ptr = calloc_ex(num, size, shm->poolptr);

    rc = robust_mutex_unlock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
end:
    return(ptr);
}

void* libsrpc_shmem_realloc(void* ptr, size_t newsize)
{
    void *nptr = NULL;
    int rc = 0;
    libsrpc_shmem_t *shm;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    if(!simplerpc_data->shmempool.shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    shm = simplerpc_data->shmempool.shm;

    rc = robust_mutex_lock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }

    nptr = realloc_ex(ptr, newsize, shm->poolptr);

    rc = robust_mutex_unlock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
end:
    return(nptr);
}

void libsrpc_shmem_free(void *ptr)
{
    int rc = 0;
    libsrpc_shmem_t *shm;

    if(!simplerpc_data) {
        __libsrpc_errno_set(ELIBNOINIT);
        goto end;
    }
    if(!simplerpc_data->shmempool.shm) {
        __libsrpc_errno_set(ESHMNOINIT);
        goto end;
    }

    shm = simplerpc_data->shmempool.shm;

    rc = robust_mutex_lock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }

    free_ex(ptr, shm->poolptr);

    rc = robust_mutex_unlock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
end:
    return;
}

int libsrpc_shmem_link(void *ptr)
{
    int rc;
    libsrpc_shmem_t *shm;

    if(!ptr || !simplerpc_data || !simplerpc_data->shmempool.shm) return(-EINVAL);
    shm = simplerpc_data->shmempool.shm;

    rc = robust_mutex_lock(shm);
    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
    rc = tlsf_link(ptr, shm->poolptr);

    rc = robust_mutex_unlock(shm);

    if(rc < 0) {
        __libsrpc_errno_set(rc);
        goto end;
    }
end:
    return(rc);
}

/* ------------------------------------------------------------------------------ */
/* Регистрация процессов в разделяемой памяти */
/* ------------------------------------------------------------------------------ */
int libsrpc_shmem_reg_pid(pid_t pid)
{
    int rc = 0;
    libsrpc_shmem_pid_t *pid_info;
    libsrpc_shmem_t *shm = simplerpc_data->shmempool.shm;

    pid_info = libsrpc_shmem_malloc(sizeof(libsrpc_shmem_pid_t));
    if(!pid_info) {
        rc = -ENOMEM;
        ERR_PRINT("libsrpc_shmem_reg_pid: malloc failed\n");
        goto err;
    }

    memset(pid_info, 0, sizeof(libsrpc_shmem_pid_t));
    pid_info->pid = pid;

    rc = sem_init(&pid_info->sem, 1, 0);
    if(rc < 0) {
        ERR_PRINT("libsrpc_shmem_reg_pid: sem_init failed: %s\n", strerror(rc));
        goto err;
    }

    mpmc_queue_init(&pid_info->queue);

    rc = list_add_tail(&shm->reg_pid_list, &pid_info->list);
    if(rc < 0) {
        ERR_PRINT("libsrpc_shmem_reg_pid: dlist_push_front failed: %s\n", strerror(rc));
        goto err;
    }
    DBG_PRINT("libsrpc_shmem_reg_pid: pid=%d registered pid_info=%p\n", pid, pid_info);
end:
    return(rc);
err:
    if(pid_info) libsrpc_shmem_free(pid_info);
    goto end;
}

int libsrpc_shmem_unreg_pid(pid_t pid)
{
    int rc = 0;
    libsrpc_shmem_pid_t *pid_info = NULL;

    pid_info = libsrpc_shmem_get_reg_pid(pid);
    if(pid_info) {
        sem_destroy(&pid_info->sem);
        srpc_thread_rcv_queue_clear(pid_info);
        libsrpc_shmem_free(pid_info);
    }
    DBG_PRINT("libsrpc_shmem_unreg_pid: pid=%d unregistered pid_info=%p\n", pid, pid_info);
    return(rc);
}

libsrpc_shmem_pid_t* libsrpc_shmem_get_reg_pid(pid_t pid)
{
    libsrpc_shmem_pid_t *pid_info = NULL;
    libsrpc_shmem_t *shm = simplerpc_data->shmempool.shm;

    int rc = list_lock(&shm->reg_pid_list);
    if (rc == 0) {
        libsrpc_shmem_pid_t *it;
        LIST_FOREACH_ENTRY(&shm->reg_pid_list, it, list, libsrpc_shmem_pid_t) {
            if(pid == it->pid) {
                pid_info = it;
                break;
            }
        }
    list_unlock(&shm->reg_pid_list);
    }
    DBG_PRINT("libsrpc_shmem_get_reg_pid: pid=%d get registered pid_info=%p\n", pid, pid_info);
    return(pid_info);
}


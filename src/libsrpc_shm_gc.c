
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <stdatomic.h>

#include "tlsf_txn.h"

#include "libsrpc_pthread.h"
#include "libsrpc_shmem.h"
#include "libsrpc_errno.h"
#include "libsrpc_debug_print.h"
#include "libsrpc_local.h"

#include "libsrpc_shm_gc.h"

static int gc_init = 0;
static atomic_int gc_stop = 0;

static void * libsrpc_shm_gc_thread(void *arg);

int libsrpc_shm_gc_init(void)
{
    int rc = 0;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    thread_flag_t flags = thread_flag_default();

    if(!shm) {
        ERR_PRINT("shm is NULL\n");
        return -ESHMNOINIT;
    }

    if(gc_init == 1) {
        return 0;
    }

    rc = libsrpc_sem_init(&shm->gc.sem_wakeup, 0);
    if(rc < 0) {
        ERR_PRINT("libsrpc_sem_init failed\n");
        return -EINVAL;
    }

    flags.flags = THREAD_FLAG_JOINABLE;
    flags.thread_out = &shm->gc.thread;
    rc = pthread_start(libsrpc_shm_gc_thread, &shm->gc, flags);
    if(rc < 0) {
        ERR_PRINT("pthread_start failed\n");
        return -EINVAL;
    }

    gc_init = 1;
    return 0;
}

int libsrpc_shm_gc_destroy(void)
{
    int rc = 0;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();

    if(!shm) {
        ERR_PRINT("shm is NULL\n");
        return -ESHMNOINIT;
    }

    if(gc_init == 0) {
        return 0;
    }

    gc_init = 0;
    atomic_store(&gc_stop, 1); // Сигнал остановки потока.
    libsrpc_shm_gc_wakeup();

    rc = pthread_join(shm->gc.thread, NULL);
    if(rc < 0) {
        ERR_PRINT("pthread_join failed\n");
    }

    rc = libsrpc_sem_destroy(&shm->gc.sem_wakeup);
    if(rc < 0) {
        ERR_PRINT("libsrpc_sem_destroy failed\n");
    }

    return 0;
}

int libsrpc_shm_gc_wakeup(void)
{
    int rc = 0;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();

    if(!shm) {
        ERR_PRINT("shm is NULL\n");
        return -ESHMNOINIT;
    }

    rc = libsrpc_sem_post(&shm->gc.sem_wakeup);

    return rc;
}

/* -------------------------------------------------------------------------- */
/* GC поток */
/* -------------------------------------------------------------------------- */

typedef struct gc_elm_s {
    void    *ptr;
    uint16_t type;
    uint16_t uid;
    uint32_t refcount;
} gc_elm_t;

#define POOL_SIZE 1024
typedef struct gc_pool_s {
    int state;
    int count;
    int type_cnt[LIBSRPC_SHMDT_MAX];
    gc_elm_t elm[POOL_SIZE];
} gc_pool_t;

static gc_pool_t gc_pool = {0};


static int destructor_fn(tlsf_t tlsf, void *ptr, block_header_t *block, uint16_t match_uid, void *user)
{
    int data_type = 0;
    int flag_dc = 0;
    gc_pool_t *pool = (gc_pool_t *)user;

    if(ptr == NULL || tlsf == NULL || block == NULL || pool == NULL) {
        return -EINVAL;
    }

    if(pool->count >= POOL_SIZE) {
        return -ENOMEM;
    }

    data_type = tlsf_get_data_type(tlsf, ptr);
    if(data_type < 0) data_type = 0;
    flag_dc = tlsf_getdc(tlsf, ptr);
    if(flag_dc < 0) flag_dc = 0;

    DBG_PRINT("!!! GC: ptr=%p, match_uid=%d, data_type=%d\n", ptr, match_uid, data_type);

    switch(data_type) {
        case LIBSRPC_SHMDT_PROC:
        case LIBSRPC_SHMDT_REGFN:
        case LIBSRPC_SHMDT_REQUEST:
            if(flag_dc) { // Специальные блоки в отложенную очистку.
                pool->elm[pool->count].ptr = ptr;
                pool->elm[pool->count].type = (uint16_t)data_type;
                pool->elm[pool->count].uid = match_uid;
                pool->elm[pool->count].refcount = 0;
                pool->count++;
                if(data_type < LIBSRPC_SHMDT_MAX) pool->type_cnt[data_type]++;
                if(pool->count >= POOL_SIZE) {
                    return -ENOMEM;
                }
            }
            break;
        case LIBSRPC_SHMDT_USER:
        default:
            tlsf_free_nb(tlsf, ptr, match_uid);
            break;
    }

    return(0);
}

static int gc_pool_destroy(libsrpc_shmem_t *shm, gc_pool_t *pool)
{
    int free_count = 0;

    if(!pool) {
        return -EINVAL;
    }

    for(int i = 0; i < pool->count; i++) {
        switch(pool->elm[i].type) {
            case LIBSRPC_SHMDT_PROC:
                break;
            case LIBSRPC_SHMDT_REGFN:
                break;
            case LIBSRPC_SHMDT_REQUEST:
                if(libsrpc_req_is_busy_proc(pool->elm[i].ptr)) {
                    pool->elm[i].refcount++;
                    //DBG_PRINT("GC: request is busy, refcount=%u\n", pool->elm[i].refcount);
                }
                break;
        }
    }

    for(int i = 0; i < pool->count; i++) {
        if(pool->elm[i].ptr != NULL || pool->elm[i].refcount == 0) {
            free_count++;
            switch(pool->elm[i].type) {
                case LIBSRPC_SHMDT_PROC:
                    break;
                case LIBSRPC_SHMDT_REGFN:
                    break;
                case LIBSRPC_SHMDT_REQUEST:
                    libsrpc_req_destroy(pool->elm[i].ptr);
                    //DBG_PRINT("GC: request destroyed, refcount=%u\n", pool->elm[i].refcount);
                    break;
                default:
                    break;
            }
        }
    }

    if(!free_count) {
        DBG_PRINT("GC: no free blocks\n");
        return 0;
    }

    /* быстро удалим под блокировкой */
    tlsf_lock((tlsf_t)shm->poolptr);
    for(int i = 0; i < pool->count; i++) {
        if(pool->elm[i].refcount == 0) {
            tlsf_free_nb((tlsf_t)shm->poolptr, pool->elm[i].ptr, pool->elm[i].uid);
            //DBG_PRINT("GC: block freed, ptr=%p, type=%d, uid=%d, refcount=%u\n", pool->elm[i].ptr, pool->elm[i].type, pool->elm[i].uid, pool->elm[i].refcount);
        }
    }
    tlsf_unlock((tlsf_t)shm->poolptr);

    return(free_count);
}


static void * libsrpc_shm_gc_thread(void *arg)
{
    libsrpc_shm_gc_t *gc = (libsrpc_shm_gc_t *)arg;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    struct timespec ts = {0};
DBG_PRINT("GC thread started\n");
    if(!shm) {
        ERR_PRINT("shm is NULL\n");
        return NULL;
    }

    while(atomic_load(&gc_stop) == 0) {
        int rc = 0;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)(1000000000U / SHM_GC_FREQ_CHECK);
        if (ts.tv_nsec >= 1000000000U) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000U;
        }

        rc = libsrpc_sem_timedwait(&gc->sem_wakeup, &ts);
        if(rc < 0) {
            if(errno != ETIMEDOUT && errno != EINTR && errno != EAGAIN) {
                ERR_PRINT("libsrpc_sem_wait failed rc=%d '%s'\n", rc, strerror(errno));
                break;
            }
        }
        if(atomic_load(&gc_stop) != 0) {
            break;
        }

        memset(&gc_pool, 0, sizeof(gc_pool));
        tlsf_free_uid_blocks((tlsf_t)shm->poolptr, 0, destructor_fn, &gc_pool);
        if(gc_pool.count == 0) continue;

        gc_pool_destroy(shm, &gc_pool);

    }
DBG_PRINT("GC thread stopped\n");
    return NULL;
}


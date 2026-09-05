#ifndef FILE_LIBSRPC_SHM_GC_H
#define FILE_LIBSRPC_SHM_GC_H

#include <pthread.h>
#include "libsrpc_wrapper.h"

// Частота проверки GC (число раз в секунду)
#ifndef SHM_GC_FREQ_CHECK
#define SHM_GC_FREQ_CHECK (10ULL)
#endif

typedef struct libsrpc_shm_gc_s {
    pthread_t       thread;
    libsrpc_sem_t   sem_wakeup;
} libsrpc_shm_gc_t;


int libsrpc_shm_gc_init(void);
int libsrpc_shm_gc_destroy(void);
int libsrpc_shm_gc_wakeup(void);


#endif // FILE_LIBSRPC_SHM_GC_H

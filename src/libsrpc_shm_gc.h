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
    libsrpc_list_head_t list_head; // Список заблокированных блоков памяти на очистку.
} libsrpc_shm_gc_t;

typedef struct libsrpc_shm_trash_s {
    libsrpc_list_node_t node; // Список блоков памяти в процессе.
    uint16_t uid;   // UID процесса.
    uint32_t num;   // Размер массива ptr[num].
    uint32_t cnt;   // Фактическое количество блоков памяти в массиве ptr[num].
    void *ptr[0];   // Указатели на блоки памяти.
} libsrpc_shm_trash_t;

int libsrpc_shm_gc_init(void);
int libsrpc_shm_gc_destroy(void);
int libsrpc_shm_gc_wakeup(void);

libsrpc_shm_trash_t * libsrpc_shm_trash_create_node(uint32_t num);
int libsrpc_shm_trash_add_node(libsrpc_shm_trash_t *trash);
int libsrpc_shm_trash_add_ptr(libsrpc_shm_trash_t **ptr_trash, void *ptr);

#endif // FILE_LIBSRPC_SHM_GC_H

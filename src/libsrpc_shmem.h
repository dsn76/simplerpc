#ifndef FILE_LIBSRPC_SHMEM_H
#define FILE_LIBSRPC_SHMEM_H

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "libsrpc_private.h"

/* Структура в разделяемой памяти */
typedef struct libsrpc_shmem_s {
    char        sign[32];       // Сигнатура, для опознания своей версии.
    srpc_regfn_shm_t    regfn;  // Реестр RPC функций в разделяемой памяти.
    list_head_t reg_pid_list;   // Список регистрации процессов в разделяемой памяти.
    uintptr_t   basevadr;       // Базовый виртуальный адрес разделяемой памяти, по умолчанию: 0x200000000000.
    size_t      shmsize;        // Размер разделяемой памяти.
    size_t      poolsize;       // Размер пула в разделяемой памяти.
    pthread_mutex_t mutex;      // robust mutex для защиты доступа к poolptr[] в разделяемой памяти между процессами
    uint32_t cnt_errownerdead;  // счётчик ошибок EOWNERDEAD (robust mutex).
    uint8_t     poolptr[] __attribute__((aligned(64)));      // указатель на начало пула в разделяемой памяти. Примечание: poolptr[] должен быть последним полем в структуре.
} libsrpc_shmem_t;


typedef struct libsrpc_shmem_pool_s {
    libsrpc_shmem_t *shm;
    size_t          shm_size;
    int             shm_fd;
} libsrpc_shmem_pool_t;


int libsrpc_shmem_create(libsrpc_shmem_pool_t *pool, char *name, size_t size, uintptr_t virtaddr);
int libsrpc_shmem_open(libsrpc_shmem_pool_t *pool, int shm_fd, uintptr_t virtaddr, const char *name);
int libsrpc_shmem_destroy(libsrpc_shmem_pool_t *pool);

void* libsrpc_shmem_malloc(size_t size);
void* libsrpc_shmem_calloc(size_t num, size_t size);
void* libsrpc_shmem_realloc(void* ptr, size_t newsize);
void  libsrpc_shmem_free(void *ptr);
int   libsrpc_shmem_link(void *ptr);

#endif // FILE_LIBSRPC_SHMEM_H

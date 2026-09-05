#ifndef FILE_LIBSRPC_H
#define FILE_LIBSRPC_H

/* ------------------------------------------------------------------------------ */
/* Публичное API библиотеки libsrpc.                                              */
/* ------------------------------------------------------------------------------ */

typedef enum libsrpc_flag_send_rpc_s {
    RPC_SEND_ALL = 0,
    RPC_SEND_FIRST = 1,
    RPC_SEND_LAST = 2,
    RPC_SEND_RR = 3,
} libsrpc_flag_send_rpc_t;

#include "libsrpc_rpc_functions.h"

/* ------------------------------------------------------------------------------ */

/* Декларация RPC функций */
#define XF(flags,rettype,name,...)   rettype name(__VA_ARGS__);
    RPC_LIST
#undef XF

/* ------------------------------------------------------------------------------ */

/* Декларации вспомогательных функций библиотеки */
void* libsrpc_shmem_malloc(size_t size);                /* Выделение памяти в разделяемой памяти */
void* libsrpc_shmem_calloc(size_t num, size_t size);    /* Выделение памяти в разделяемой памяти */
void* libsrpc_shmem_realloc(void* ptr, size_t newsize); /* Перераспределение памяти в разделяемой памяти */
void  libsrpc_shmem_free(void *ptr);                    /* Освобождение памяти в разделяемой памяти */
int   libsrpc_shmem_link(void *ptr);                    /* Связывание выделенной разделяемой памяти со своим процессом, защита от освобождения памяти другим процессом */

/* Опциональная замена стандартного аллокатора на аллокатор разделяемой памяти */
#ifndef LIBSRPC_DISABLE_SUBSTITUTION_ALLOCATOR
void srpc_alloc_sw_std(void);                          /* Переключение на стандартный аллокатор */
void srpc_alloc_sw_shm(void);                          /* Переключение на аллокатор разделяемой памяти */
#endif

/* Получение количества результатов последнего запроса. Возвращает количество ответивших исполнителей. */
int libsrpc_lastreq_num(void);
/* Получение результата последнего запроса. idx - индекс исполнителя, retval - указатель на буфер для результата, sz - размер буфера. Возвращает код ошибки. */
int libsrpc_lastreq_get(int idx, void *retval, size_t sz); 

int libsrpc_errno_get(void);                            /* Получение кода последней ошибки для void функций */
const char*  libsrpc_strerror(int errnum);              /* Получение строки ошибки по коду ошибки */

#endif // FILE_LIBSRPC_H

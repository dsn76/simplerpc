#ifndef FILE_LIBSRPC_H
#define FILE_LIBSRPC_H

#include "libsrpc_rpc_functions.h"

/* ------------------------------------------------------------------------------ */

/* Декларация RPC функций */
#define XF(rettype,name,...)   rettype name(__VA_ARGS__);
    RPC_LIST
#undef XF

/* ------------------------------------------------------------------------------ */

/* Декларации вспомогательных функций библиотеки */
void* libsrpc_shmem_malloc(size_t size); /* Выделение памяти в разделяемой памяти */
void* libsrpc_shmem_calloc(size_t num, size_t size); /* Выделение памяти в разделяемой памяти */
void* libsrpc_shmem_realloc(void* ptr, size_t newsize); /* Перераспределение памяти в разделяемой памяти */
void  libsrpc_shmem_free(void *ptr); /* Освобождение памяти в разделяемой памяти */
int   libsrpc_shmem_link(void *ptr); /* Связывание выделенной разделяемой памяти со своим процессом, защита от освобождения памяти другим процессом */
#ifndef LIBSRPC_DISABLE_SUBSTITUTION_ALLOCATOR
void srpc_alloc_sw_std(void);
void srpc_alloc_sw_shm(void);
#endif

int libsrpc_lastreq_get(int idx, void *retval, size_t sz); /* Получение результата последнего запроса. idx - индекс исполнителя, retval - указатель на буфер для результата, sz - размер буфера. Возвращает код ошибки. */

int libsrpc_errno_get(void);
const char*  libsrpc_strerror(int errnum);

#endif // FILE_LIBSRPC_H

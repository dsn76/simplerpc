#ifndef FILE_LIBSRPC_H
#define FILE_LIBSRPC_H

#include <stdint.h>
#include <sys/types.h>

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

/* Определение идентификаторов экспортируемых RPC функций */
typedef enum e_libsrpc_funid {
    LIBSRPC_FUNID_START = 16,
    #define XF(flags,rettype,name,...)   libsrpc_funid_##name,
        RPC_LIST
    #undef XF
    LIBSRPC_FUNID_MAX
} libsrpc_funid_t;

/* ------------------------------------------------------------------------------ */

/* Декларация RPC функций */
#define XF(flags,rettype,name,...)   rettype name(__VA_ARGS__);
    RPC_LIST
#undef XF

/* ------------------------------------------------------------------------------ */
/* Декларации вспомогательных функций библиотеки */
/* ------------------------------------------------------------------------------ */
/* Выделение памяти в разделяемой памяти */
void* libsrpc_shmem_malloc(size_t size);
/* Выделение памяти в разделяемой памяти */
void* libsrpc_shmem_calloc(size_t num, size_t size);
/* Перераспределение памяти в разделяемой памяти */
void* libsrpc_shmem_realloc(void* ptr, size_t newsize);
/* Освобождение памяти в разделяемой памяти */
void  libsrpc_shmem_free(void *ptr);
/* Связывание выделенной разделяемой памяти со своим процессом, защита от освобождения памяти другим процессом */
int   libsrpc_shmem_link(void *ptr);
/* Проверка существования выделенной разделяемой памяти и получение её размера */
int   libsrpc_shmem_get_size(void *ptr, size_t *psz);

/* Опциональная замена стандартного аллокатора на аллокатор разделяемой памяти */
#ifndef LIBSRPC_DISABLE_SUBSTITUTION_ALLOCATOR
/* Переключение на стандартный аллокатор */
void libsrpc_alloc_sw_std(void);
/* Переключение на аллокатор разделяемой памяти */
void libsrpc_alloc_sw_shm(void);
#endif

/* Установка таймаута для ближайшего вызова RPC функции (приоритет высокий). */
void libsrpc_timeout_oneshot_set(uint64_t timeout);
/* Установка таймаута для конкретной экспортируемой RPC функции (приоритет средний). */
void libsrpc_timeout_func_set(libsrpc_funid_t funid, uint64_t timeout);
/* Установка таймаута для всех RPC функций (приоритет низкий). */
void libsrpc_timeout_global_set(uint64_t timeout);

/* Получение количества зарегистрированных исполнителей для конкретной RPC функции. */
int libsrpc_fnreg_num_get(libsrpc_funid_t funid);

/* Получение количества результатов последнего запроса. Возвращает количество ответивших исполнителей. */
int libsrpc_lastreq_num(void);
/* Получение результата последнего запроса. idx - индекс исполнителя, retval - указатель на буфер для результата, sz - размер буфера. Возвращает код ошибки. */
int libsrpc_lastreq_get(int idx, void *retval, size_t sz);

/* Блокировка освобождения GC разделяемой памяти процесса. idx - индекс исполнителя, ptr - указатель на буфер для результата. Возвращает отрицательный код ошибки. */
int libsrpc_shmem_proc_lock(int idx, void *ptr);
/* Разблокировка освобождения GC разделяемой памяти процесса. slot - индекс исполнителя. Возвращает отрицательный код ошибки. */
int libsrpc_shmem_proc_unlock(int slot);
/* Разблокировка освобождения GC разделяемой памяти процесса. Возвращает отрицательный код ошибки. */
int libsrpc_shmem_proc_all_unlock(void);

/* Получение кода последней ошибки для void функций */
int libsrpc_errno_get(void);
/* Получение строкового представления кода последней ошибки */
const char*  libsrpc_strerror(int errnum);

#endif // FILE_LIBSRPC_H

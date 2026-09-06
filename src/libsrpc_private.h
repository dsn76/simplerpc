#ifndef FILE_LIBSRPC_PRIVATE_H
#define FILE_LIBSRPC_PRIVATE_H

#include <pthread.h>
#include <semaphore.h>

#include "libsrpc.h"
#include "lf_mpmc_queue.h"

/* ------------------------------------------------------------------------------ */

/* Определение идентификаторов RPC функций */
enum e_sRPCFNID {
    sRPCFNID_START = 16,
    #define XF(flags,rettype,name,...)   sRPCFNID_##name,
        RPC_LIST
    #undef XF
    sRPCFNID_MAX
};

/* ------------------------------------------------------------------------------ */

/* Макросы для преобразований */
#define GET_FNID(name)  sRPCFNID_##name
#define sRPCFN(name)    librpcimp_##name
#define sRPC_FNNUM      (sRPCFNID_MAX - sRPCFNID_START - 1)
#define sRPC_IDX2ID(idx)   ((idx) + sRPCFNID_START + 1)
#define sRPC_ID2IDX(id)   ((id) - sRPCFNID_START - 1)
#define sRPC_BmpFuncSz   (((sRPC_FNNUM)+63) / 64) /* размер битовой карты для хранения функций. */

/* ------------------------------------------------------------------------------ */

typedef struct libsrpc_proc_s libsrpc_proc_t;

#define LIBSRPC_REGFN_SIGN  (0x1F971A85U)
#define LIBSRPC_RFEB_SZ     (32U)
typedef struct srpc_regfn_ext_block_s {
    _Atomic(uint32_t)    sign; // сигнатура блока.
    uint32_t             szfn; // Размер блока в указателях на процессы.
    size_t               size; // размер блока в байтах.
    _Atomic(libsrpc_proc_t *) proc[0]; // массив указателей на структуры libsrpc_proc_t для каждого процесса.
} srpc_regfn_ext_block_t;

#define LIBSRPC_RFMB_SZ     (8U)
typedef struct srpc_regfn_main_block_s {
    _Atomic(libsrpc_proc_t *)proc[LIBSRPC_RFMB_SZ]; // массив указателей на структуры srpc_pid_t для каждого процесса.
} srpc_regfn_main_block_t;

typedef struct libsrpc_rpc_func_s libsrpc_rpc_func_t;
/* Структура для регистрации и вызова RPC функций через разделяемую память */
typedef struct srpc_regfn_shm_s {
    struct libsrpc_rpc_func_s {
        atomic_int                          num_all; // Общее количество зарегистрированных процессов для вызова этой RPC функции.
        _Atomic(srpc_regfn_ext_block_t *)   ext_block; // указатель на следующую структуру в списке.
        srpc_regfn_main_block_t             main_block; // блок регистрации процессов для вызова этой RPC функции.
    } funcs[sRPC_FNNUM];
} srpc_regfn_shm_t;

typedef struct srpc_bmp_func_s {
    uint64_t bmp[sRPC_BmpFuncSz]; // битовая карта для хранения слинкованных процессом функций.
} srpc_bmp_func_t;

/* ------------------------------------------------------------------------------ */

#endif // FILE_LIBSRPC_PRIVATE_H

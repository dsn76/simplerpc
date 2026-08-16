#ifndef FILE_LIBSRPC_H
#define FILE_LIBSRPC_H

#include "libsrpc_rpc_functions.h"

/* ------------------------------------------------------------------------------ */

/* Определение идентификаторов RPC функций */
enum e_sRPCFNID {
    sRPCFNID_START = 16,
    #define XF(rettype,name,...)   sRPCFNID_##name,
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

/* ------------------------------------------------------------------------------ */

/* Декларация RPC функций */
#define XF(rettype,name,...)   rettype name(__VA_ARGS__);
    RPC_LIST
#undef XF

/* ------------------------------------------------------------------------------ */

/* Декларации вспомогательных функций библиотеки */
void* libsrpc_shmem_malloc(size_t size);
void* libsrpc_shmem_calloc(size_t num, size_t size);
void* libsrpc_shmem_realloc(void* ptr, size_t newsize);
void  libsrpc_shmem_free(void *ptr);
int   libsrpc_shmem_link(void *ptr);

#endif // FILE_LIBSRPC_H

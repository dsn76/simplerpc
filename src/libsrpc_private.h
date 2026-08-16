#ifndef FILE_LIBSRPC_PRIVATE_H
#define FILE_LIBSRPC_PRIVATE_H

#include <pthread.h>
#include <semaphore.h>

#include "libsrpc.h"
#include "libsrpc_mpmcq.h"

/* ------------------------------------------------------------------------------ */
/*
typedef struct srpc_pid_s {
    pid_t pid;
    int funid;
    sem_t sem_call; // семафор для пробуждения потоков обработчиков вызова RPC функций
    MpmcQueue *queue; // очередь для передачи запросов о вызовах RPC функций между процессами
} srpc_pid_t;
*/

#define SRPC_MAX_PID 8
typedef struct libsrpc_shmem_pid_s libsrpc_shmem_pid_t;
typedef struct srpc_regfn_block_s srpc_regfn_block_t;
typedef struct srpc_regfn_block_s {
    srpc_regfn_block_t *next; // указатель на следующую структуру в списке.
    atomic_int num; // количество процессов, зарегистрированных в этом блоке регистрации.
    libsrpc_shmem_pid_t *pids[SRPC_MAX_PID]; // массив указателей на структуры srpc_pid_t для каждого процесса.
} srpc_regfn_block_t;

/* Структура для регистрации и вызова RPC функций через разделяемую память */
typedef struct srpc_regfn_shm_s {
        pthread_mutex_t mutex;  // robust mutex для защиты доступа к этои и вложенным структурам.
    struct srpc_rpc_s {
        atomic_int num_all; // Общее количество зарегистрированных процессов для вызова этой RPC функции.
        srpc_regfn_block_t block; // блок регистрации процессов для вызова этой RPC функции.
    } funcs[sRPC_FNNUM];
} srpc_regfn_shm_t;

/* ------------------------------------------------------------------------------ */

#endif // FILE_LIBSRPC_PRIVATE_H

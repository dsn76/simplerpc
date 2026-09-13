#ifndef FILE_LIBSRPC_PROC_H
#define FILE_LIBSRPC_PROC_H

#include "libsrpc_wrapper.h"
#include "libsrpc_list_spin.h"


#define DAEMON_PROC_UID 1

enum e_libsrpc_proc_status {
    LIBSRPC_PROC_STATUS_INIT = 0,
    LIBSRPC_PROC_STATUS_RUN,
    LIBSRPC_PROC_STATUS_STOP,
    LIBSRPC_PROC_STATUS_ERROR,
    LIBSRPC_PROC_STATUS_EXIT,
    LIBSRPC_PROC_STATUS_DEAD,
    LIBSRPC_PROC_STATUS_DELETE,
    LIBSRPC_PROC_STATUS_MAX,
};



/* Структура потока процесса */
typedef struct libsrpc_proc_thread_s {
    _Atomic(void *) hp_req; // Hazard Pointer для request.
    pthread_t tid; // Дескриптор потока.
} libsrpc_proc_thread_t;
/* на эту структуру будет указывать локальный для потока указатель thread_req_current, для быстрого доступа к структуре запроса */

#define LIBSRPC_PROC_SIGN (0xEF1212EFU)
/* Структура процесса */
typedef struct libsrpc_proc_s {
    libsrpc_list_node_t proc_node;  // Список процессов.
    atomic_uint_least32_t sign;     // Сигнатура процесса.
    atomic_uint_least16_t status;   // Статус процесса.
    uint16_t        proc_uid;       // Идентификатор процесса внутри sRPC для маркировки блоков в TLSF.
    pid_t           pid;            // PID процесса.
    _Atomic(unsigned int) threads_run;    // Количество запущенных потоков в процессе.
    _Atomic(unsigned int) threads_wait;   // Количество ожидающих потоков в процессе.
    libsrpc_sem_t   sem_wakeup;     // Пробуждение потоков.
    libsrpc_list_head_t req_head;   // Список структур запросов в процессе, для обхода уборщиком мусора (RCU Hazard Pointer).
    lf_mpmc_queue_t queue;          // Очередь запросов в процессе.
    unsigned int    threads_num;    // Ожидаемое количество потоков в процессе.
    libsrpc_proc_thread_t threads[0]; // threads[threads_num] - Массив потоков процесса == число ядер процессора, вычисляется на этапе выделения памяти для процесса.
} libsrpc_proc_t;

/* TODO:
 * Добавить в структуру запроса сигнатуру, по которой отвечающий будет проверять запрос на существование.
 * Добавить в структуру запроса указатель на Hazard Pointer.
 * Добавить в структуру запроса libsrpc_list_node_t node для списка запросов.
 */


int libsrpc_proc_create(pid_t pid);
int libsrpc_proc_destroy(pid_t pid);
libsrpc_proc_t * libsrpc_proc_get(pid_t pid);

static inline void libsrpc_proc_set_run(libsrpc_proc_t *proc)
{
    atomic_store_explicit(&proc->status, LIBSRPC_PROC_STATUS_RUN, memory_order_release);
}

static inline void libsrpc_proc_set_stop(libsrpc_proc_t *proc)
{
    atomic_store_explicit(&proc->status, LIBSRPC_PROC_STATUS_STOP, memory_order_release);
}

static inline int libsrpc_proc_is_valid(libsrpc_proc_t *proc)
{
    return( proc != NULL &&
            atomic_load_explicit(&proc->sign, memory_order_acquire) == LIBSRPC_PROC_SIGN &&
            atomic_load_explicit(&proc->status, memory_order_acquire) == LIBSRPC_PROC_STATUS_RUN
        );
}

#endif // FILE_LIBSRPC_PROC_H

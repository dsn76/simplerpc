#ifndef FILE_LIBSRPC_LOCAL_H
#define FILE_LIBSRPC_LOCAL_H

#include <stdatomic.h>  // Для атомарных операций
#include <stdbool.h>

#include "libsrpc_debug_print.h"
#include "libsrpc_unix_socket.h"
#include "libsrpc_mpmcq.h"
#include "libsrpc_list.h"
#include "libsrpc_shmem.h"

#define CACHELINESIZE   (64)
#define SHMEM_BASE_VADR (0x200000000000)
#define SHMEM_SIZE      (2*1024*1024)

typedef struct srpc_func_s {
  const void *rpc; // Указатель на функцию в библиотеке libsrpc.
  const void *loc; // Указатель на функцию в локальном приложении.
  const char *name; // Имя функции.
} srpc_func_t; // Структура описания функции.


typedef struct libsrpc_shmem_pid_s libsrpc_shmem_pid_t;
typedef struct simplerpc_s {
  char sign[64];
  void* (*real_malloc)(size_t);
  void (*real_free)(void*);
  void* (*real_calloc)(size_t, size_t);
  void* (*real_realloc)(void*, size_t);
  atomic_char sw_malloc;
  atomic_char init_mempool;
  void *mempool;
  size_t mempool_size;

  const srpc_func_t *fn;
  const size_t fn_sz;
  atomic_bool rpc_enable; // Разрешение RPC вызовов после успешной инициализации и при отсутствии ошибок.
  libsrpc_shmem_pid_t *pid_info; // Информация о процессе в разделяемой памяти.

  libsrpc_shmem_pool_t shmempool;
  bool is_daemon;
  libsrpc_server_t srv;
  libsrpc_client_t cli;
} __attribute__((aligned(CACHELINESIZE))) simplerpc_t;


typedef struct libsrpc_shmem_pid_s {
  list_node_t list; // Элемент списка регистрации процессов.
  pid_t pid; // PID процесса.
  sem_t sem; // Пробуждение потоков.
  atomic_int threads_wait; // Количество ожидающих потоков в процессе.
  atomic_int threads_run; // Количество запущенных потоков в процессе.
  MpmcQueue queue; // Очередь запросов в процессе.
} libsrpc_shmem_pid_t;

typedef struct libsrpc_request_s {
  int funid;
  int wakeup; // futex для пробуждения вызывающего потока.

  char buf[];
} libsrpc_request_t;

extern simplerpc_t *simplerpc_data;

int libsrpc_shmem_reg_pid(pid_t pid);
int libsrpc_shmem_unreg_pid(pid_t pid);
libsrpc_shmem_pid_t* libsrpc_shmem_get_reg_pid(pid_t pid);
int srpc_thread_rcv_req_start(void);
int srpc_thread_rcv_req_stop(void);
void srpc_thread_rcv_queue_clear(libsrpc_shmem_pid_t *pi);
int srpc_regfn_rpc_shm(libsrpc_shmem_pid_t *fpid);
int srpc_unregfn_rpc_shm(libsrpc_shmem_pid_t *fpid);


#endif // FILE_LIBSRPC_LOCAL_H

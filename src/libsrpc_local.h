#ifndef FILE_LIBSRPC_LOCAL_H
#define FILE_LIBSRPC_LOCAL_H

#include <stdatomic.h>  // Для атомарных операций
#include <stdbool.h>

#include "libsrpc_debug_print.h"
#include "libsrpc_unix_socket.h"
#include "libsrpc_mpmcq.h"
#include "libsrpc_shmem.h"

#define CACHELINESIZE   (64)
#ifndef SHMEM_BASE_VADR
#define SHMEM_BASE_VADR (0x200000000000ULL)
#endif
#ifndef SHMEM_SIZE
#define SHMEM_SIZE      (2ULL*1024ULL*1024ULL)
#endif

typedef struct srpc_func_s {
  const void *rpc; // Указатель на функцию в библиотеке libsrpc.
  const void *loc; // Указатель на функцию в локальном приложении.
  const char *name; // Имя функции.
} srpc_func_t; // Структура описания функции.


//typedef struct libsrpc_shmem_pid_s libsrpc_shmem_pid_t;
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
  uint16_t proc_uid; // UID процесса. Для демона = DAEMON_PROC_UID.

  const srpc_func_t *fn;
  const size_t fn_sz;
  atomic_bool rpc_enable; // Разрешение RPC вызовов после успешной инициализации и при отсутствии ошибок.
//  libsrpc_shmem_pid_t *pid_info; // Информация о процессе в разделяемой памяти.
  libsrpc_proc_t *proc; // Информация о процессе в разделяемой памяти.

  libsrpc_shmem_pool_t shmempool;
  bool is_daemon;
  libsrpc_server_t srv;
  libsrpc_client_t cli;
} __attribute__((aligned(CACHELINESIZE))) simplerpc_t;

#define LIBSRPC_RC_FLAG_READY (0x80000000U) /* Флаг готовности ответа */
#define LIBSRPC_RC_MASK_READY (0x7FFFFFFFU) /* Маска для получения кода возврата */
typedef int srpc_rc_t; // Внутренний код возврата при выполнении RPC запроса, возникший в исполнителе.
typedef struct libsrpc_response_s {
  _Atomic(libsrpc_proc_t *) hp_proc;   // От кого ожидаем ответ, после получения ответа, Hazard Pointer будет установлен на NULL.
  _Atomic(srpc_rc_t)        rc;       // Внутренний код возврата при выполнении RPC запроса, возникший в исполнителе, или при его вызове.
  unsigned char             buf[0];   // Буфер данных с ответом.
} libsrpc_response_t;

#define LIBSRPC_REQ_SIGN (0x741B8CD7U)
typedef struct libsrpc_request_s {
  libsrpc_list_node_t node;       // Список запросов в процессе.
  libsrpc_sem_t       sem_wakeup; // Пробуждение вызывающего потока.
  _Atomic(void *)     hp_regfn;   // Hazard Pointer для реестра функций.
  _Atomic(uint32_t)   sign;       // Сигнатура запроса.
  int                 funid;      // ID функции.
  unsigned int        bufsz;      // Размер буфера данных.
  unsigned int        retoff;     // Позиция в буфере для размещения первого ответа.
  unsigned int        retsz;      // Размер одного ответа.
  unsigned int        retnum;     // Количество ответов в буфере.
  unsigned char       buf[0];     // Буфер данных.
} libsrpc_request_t;


extern simplerpc_t *simplerpc_data;
extern const char *libsrpc_daemon_name;

int libsrpc_thread_rcv_req_start(void);
int libsrpc_thread_rcv_req_stop(void);
void libsrpc_thread_rcv_queue_clear(libsrpc_proc_t *proc);
int libsrpc_req_is_busy_proc(libsrpc_request_t *req);

void libsrpc_bmp_func_set(srpc_bmp_func_t *bmp);
int libsrpc_reg_func_form_bmp(libsrpc_proc_t *proc, srpc_bmp_func_t *bmp);
int libsrpc_unreg_func_proc(libsrpc_proc_t *proc);


static inline int libsrpc_req_destroy(libsrpc_request_t *req)
{
  if(!req) {
    return(-EINVAL);
  }
  uint32_t expected = LIBSRPC_REQ_SIGN;
  if(!atomic_compare_exchange_strong(&req->sign, &expected, 0)) {
    return(-EALREADY);  // уже уничтожается или уничтожен
  }
  // Теперь sign == 0 — sem_post в queue_clear проверит sign и пропустит
  libsrpc_sem_destroy(&req->sem_wakeup);
  req->retnum = 0;
  req->retsz = 0;
  return(0);
}

static inline int libsrpc_req_is_corrupted(libsrpc_request_t *req)
{
  unsigned int size = 0;

  if(!req) {
    return(1);
  }
  libsrpc_request_t req_tmp = *req;

  if(atomic_load_explicit(&req->sign, memory_order_acquire) != LIBSRPC_REQ_SIGN) {
    return(1);
  }
  if(!req_tmp.retnum || !req_tmp.retsz) {
    return(1);
  }
  size = sizeof(libsrpc_request_t) + req_tmp.retoff + req_tmp.retsz * req_tmp.retnum;
  if(req_tmp.bufsz < size) {
    return(1);
  }
  if(atomic_load_explicit(&req->sign, memory_order_acquire) != LIBSRPC_REQ_SIGN) {
    return(1);
  }
  return(0);
}

static inline libsrpc_response_t * libsrpc_req_get_response(libsrpc_request_t *req, unsigned int idx)
{
  if(libsrpc_req_is_corrupted(req) || idx >= req->retnum) {
    return(NULL);
  }
  libsrpc_request_t req_tmp = *req;
  unsigned int offset = req_tmp.retoff + req_tmp.retsz * idx;
  return((libsrpc_response_t *)&req->buf[offset]);
}

#endif // FILE_LIBSRPC_LOCAL_H

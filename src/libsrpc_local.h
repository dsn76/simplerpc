#ifndef FILE_LIBSRPC_LOCAL_H
#define FILE_LIBSRPC_LOCAL_H

#include <stdatomic.h>  // Для атомарных операций
#include <stdbool.h>

#include "libsrpc_debug_print.h"
#include "libsrpc_unix_socket.h"
#include "lf_mpmc_queue.h"
#include "libsrpc_shmem.h"

#define CACHELINESIZE   (64)
#ifndef SHMEM_BASE_VADR
#define SHMEM_BASE_VADR (0x200000000000ULL)
#endif
#ifndef SHMEM_SIZE
#define SHMEM_SIZE      (2ULL*1024ULL*1024ULL)
#endif

#define LIBSRPC_TIMEOUT_DEFAULT 1000000 // 1 000 000 микросекунд = 1 секунда.
#define LIBSRPC_TIMEOUT_MINIMUM 100 // 0.1 миллисекунда.


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

typedef enum srpc_rc_staus_e { RC_ST_EMPTY = 0, RC_ST_LOCK, RC_ST_READY, RC_ST_ERROR } srpc_rc_staus_t;

typedef union srpc_rc_u {
  int rc;
  struct {
    int code:30; // Код возврата.
    unsigned int st:2; // Статус ответа.
  };
} srpc_rc_t; // Внутренний код возврата при выполнении RPC запроса, возникший в исполнителе.

typedef struct libsrpc_response_s {
  _Atomic(libsrpc_proc_t *) hp_proc;   // От кого ожидаем ответ, после получения ответа, Hazard Pointer будет установлен на NULL.
  _Atomic(srpc_rc_t)        rc;       // Внутренний код возврата при выполнении RPC запроса, возникший в исполнителе, или при его вызове.
  uint16_t                  uid;      // UID процесса, от которого ожидаем ответ, для проверки ABA.
  unsigned char             buf[0];   // Буфер данных с ответом.
} libsrpc_response_t;

typedef union libsrpc_gc_lock_u {
  uint64_t lock_all; // Блокировка очистки памяти по всем UID процесса от GC.
  uint16_t lock[4];  // Блокировка очистки памяти по UID процесса от GC.
} libsrpc_gc_lock_t;

#define LIBSRPC_REQ_SIGN (0x741B8CD7U)
typedef uint64_t libsrpc_req_ctrl_t;
typedef struct libsrpc_request_s {
  libsrpc_list_node_t node;       // Список запросов в процессе.
  libsrpc_sem_t       sem_wakeup; // Пробуждение вызывающего потока.
  _Atomic(void *)     hp_regfn;   // Hazard Pointer для реестра функций.
  union {
    _Atomic(libsrpc_req_ctrl_t) control;   // Контрольные поля запроса.
    struct {
      _Atomic(uint32_t) sign;     // Сигнатура запроса.
              uint32_t  seq_num;  // Номер последовательности запроса.
    };
  };
  _Atomic(libsrpc_gc_lock_t)   gc_lock_uid; // Блокировка очистки памяти UID процессов от GC.
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

/* Проверка блокировки освобождения GC разделяемой памяти процесса. uid - UID процесса. Возвращает 1 если блокировка установлена, 0 если нет. */
int libsrpc_shmem_check_uid_lock(libsrpc_request_t *req, int uid);
/* Проверка блокировки освобождения GC разделяемой памяти по UID процесса. uid - UID процесса. Возвращает 1 если блокировка установлена, 0 если нет. */
int libsrpc_shmem_is_locked_uid(uint16_t uid);

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

static inline int libsrpc_req_is_corrupted(libsrpc_request_t *req, libsrpc_req_ctrl_t *ctrl)
{
  size_t size = 0;

  if(!req) {
    return(1);
  }
  libsrpc_request_t req_tmp = *req;

  if(ctrl && *ctrl != req_tmp.control) {
    return(1);
  }
  if(req_tmp.sign != LIBSRPC_REQ_SIGN) {
    return(1);
  }
  if(!req_tmp.retnum || !req_tmp.retsz) {
    return(1);
  }
  size = sizeof(libsrpc_request_t) + (size_t)req_tmp.retoff + (size_t)req_tmp.retsz * (size_t)req_tmp.retnum;
  if(req_tmp.bufsz < size) {
    return(1);
  }
  if(ctrl && atomic_load_explicit(&req->control, memory_order_acquire) != *ctrl) {
    return(1);
  }
  if(atomic_load_explicit(&req->sign, memory_order_acquire) != LIBSRPC_REQ_SIGN) {
    return(1);
  }
  return(0);
}

static inline libsrpc_response_t * libsrpc_req_get_response_fast(libsrpc_request_t *req, unsigned int idx)
{
  if(!req) {
    return(NULL);
  }
  libsrpc_request_t req_tmp = *req;
  size_t size = sizeof(libsrpc_request_t) + (size_t)req_tmp.retoff + (size_t)req_tmp.retsz * (size_t)req_tmp.retnum;
  if(req_tmp.bufsz < size) {
    return(NULL);
  }
  if(idx >= req_tmp.retnum) {
    return(NULL);
  }
  unsigned int offset = req_tmp.retoff + req_tmp.retsz * idx;
  return((libsrpc_response_t *)&req->buf[offset]);
}

static inline libsrpc_response_t * libsrpc_req_get_response(libsrpc_request_t *req, unsigned int idx)
{
  if(!req) {
    return(NULL);
  }
  libsrpc_request_t req_tmp = *req;
  unsigned int offset = req_tmp.retoff + req_tmp.retsz * idx;
  if(libsrpc_req_is_corrupted(req, NULL) || idx >= req->retnum) {
    return(NULL);
  }
  return((libsrpc_response_t *)&req->buf[offset]);
}

static inline int libsrpc_req_response_set(libsrpc_request_t *req, libsrpc_response_t *resp, libsrpc_req_ctrl_t *ctrl, void *val, size_t sz)
{
  srpc_rc_t expected = {0};
  if(libsrpc_req_is_corrupted(req, ctrl)) {
    return(-EINVAL);
  }
  if(!atomic_compare_exchange_strong_explicit(&resp->rc, &expected, (srpc_rc_t){.st = RC_ST_LOCK}, memory_order_acquire, memory_order_relaxed)) {
    return(-EBUSY);
  }
  if(val && sz > 0) {
    memcpy(&resp->buf[0], val, sz);
  }
  atomic_store_explicit(&resp->rc, (srpc_rc_t){.st = RC_ST_READY}, memory_order_release);
  return(((srpc_rc_t){.st = RC_ST_READY}).rc);
}

static inline int libsrpc_req_response_get(libsrpc_response_t *resp, void *val, size_t sz)
{
  if(!resp) {
    return(-EINVAL);
  }
  for(int count = 0; count < 1024; count++) {
    srpc_rc_t final_rc = atomic_load_explicit(&resp->rc, memory_order_acquire);
    if(final_rc.st == RC_ST_LOCK) continue;
    if(final_rc.st == RC_ST_ERROR) {
      return(final_rc.code < 0 ? final_rc.code : -EBADFD);
    }
    if(final_rc.st == RC_ST_READY) {
      if(val && sz > 0) memcpy(val, &resp->buf, sz);
      return(final_rc.code);
    }
    __builtin_ia32_pause();
  }
  __libsrpc_errno_set(ETIMEDOUT);
  return(-ETIMEDOUT);
}

#endif // FILE_LIBSRPC_LOCAL_H

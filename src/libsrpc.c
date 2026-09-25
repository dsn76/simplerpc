
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <stdatomic.h>

#include <tlsf_txn.h>

#include "libsrpc.h"
#include "libsrpc_local.h"
#include "libsrpc_errno.h"
#include "libsrpc_debug_print.h"
#include "libsrpc_pthread.h"
#include "lf_mpmc_queue.h"
#include "libsrpc_wrapper.h"
#include "libsrpc_daemon.h"


/* Вся эта препроцессорная магия нужна, потому что стандартизаторы Си,
 * до сих пор не осилили нормальную препроцессорную кодогенерацию для: сериализации/десериализации параметров функций,
 * полей структур, итп. Даже простой битовый циклический сдвиг - в Си отсутствует как отдельная операция.
 */
/* ============================================================================== */
/* Макросы для генерации аргументов функций, тела функций и размера буфера запроса. */
/* ------------------------------------------------------------------------------ */
#include "macro.h"
#define M_GENARG(t,v,f) t v COMMA_IF(f)
#define M_ARGFUN(...) EVAL(FOREACH2(M_GENARG,p,,__VA_ARGS__))
#define M_BODYGEN(t,v,f)  memcpy(&req->buf[pos], &v, sizeof(v)); pos += sizeof(v);
#define M_BODYFUN(...) EVAL(FOREACH2(M_BODYGEN,p,,__VA_ARGS__))
#define M_REQSZGEN(t)  sizeof(t) +
#define M_REQSIZE(...) EVAL(FOREACH1(M_REQSZGEN,__VA_ARGS__))

/* ------------------------------------------------------------------------------ */
// Определяем макрос сравнения ТОЛЬКО для void. 
// Для всех остальных типов соответствующего макроса не будет.
#define COMPARE_void(x) x
#define IS_COMPARABLE(x) IS_PAREN(CAT(COMPARE_, x) (()))
#define PRIMITIVE_COMPARE(x, y) IS_PAREN(COMPARE_ ## x ( COMPARE_ ## y ) ( (()) ))
#define NOT_EQUAL(x, y) \
    IIF(BITAND(IS_COMPARABLE(x))(IS_COMPARABLE(y))) \
    ( \
        PRIMITIVE_COMPARE, \
        1 EAT \
    )(x, y)
#define EQUAL(x, y) COMPL(NOT_EQUAL(x, y))

// return в зависимости от типа.
#define RETDATA(type, data) EVAL(IIF(EQUAL(type,void))(return,return(*(type*)(data))))
#define RLEN(type) IIF(EQUAL(type, void))(0, sizeof(type))

#define ALIGNLONG(x) (((size_t)(x) + sizeof(long) - 1) & ~(sizeof(long) - 1))
/* ============================================================================== */

/* Определяет версию библиотеки, приложения слинкованные с разными версиями не будут взаимодействовать друг с другом.
 * Это защита от несовместимости версий библиотеки и приложения. */
#ifndef BUILD_TS
#define BUILD_TS "00000000000000" // Значение по умолчанию
#endif
const char *libsrpc_daemon_name = "simplerpc_" BUILD_TS;

static simplerpc_t srpc;
simplerpc_t *simplerpc_data = &srpc;
/* ============================================================================== */
// Флаг для отключения рекурсии при вызове RPC функций.
_Thread_local static atomic_char srpc_disable_rpc_recursion = 0;
_Thread_local static libsrpc_request_t *current_req = NULL; // Текущий блок запроса.
_Thread_local static uint64_t rpc_timeout_oneshot = 0;  // Одиночный таймаут для ближайшего вызова RPC функции (приоритет высокий). 
_Thread_local static uint64_t rpc_timeout[sRPC_FNNUM] = {0}; // Таймауты для каждой RPC функции (приоритет средний).
static _Atomic(uint64_t) rpc_timeout_global = LIBSRPC_TIMEOUT_DEFAULT; // 1 секунда. Таймаут для всех RPC функций (приоритет низкий).
/* ------------------------------------------------------------------------------ */

static void libsrpc_req_dump_print(libsrpc_request_t *req __attribute__((unused)))
{
  DBG_PRINT("req: %p sign: %X funid: %d bufsz: %d retoff: %d retsz: %d retnum: %d\n", (void*)req, req->sign, req->funid, req->bufsz, req->retoff, req->retsz, req->retnum);
}

/* ------------------------------------------------------------------------------ */
void libsrpc_timeout_func_set(libsrpc_funid_t funid, uint64_t timeout)
{
  int funidx = sRPC_ID2IDX(funid);
  if(funidx < 0 || funidx >= sRPC_FNNUM) {
    return;
  }
  rpc_timeout[funidx] = timeout;
}

void libsrpc_timeout_oneshot_set(uint64_t timeout)
{
  rpc_timeout_oneshot = timeout;
}

void libsrpc_timeout_global_set(uint64_t timeout)
{
  atomic_store_explicit(&rpc_timeout_global, timeout, memory_order_release);
}

/* ------------------------------------------------------------------------------ */
/* Деструктор данных потока, предотвращает утечку памяти при выходе из потока. */

static int libsrpc_req_is_bad(libsrpc_request_t *req);
static void libsrpc_req_free(libsrpc_request_t *req);
static pthread_key_t libsrpc_key;
static pthread_once_t libsrpc_key_once = PTHREAD_ONCE_INIT;

static void libsrpc_cleanup_data(void *ptr)
{
  if(!ptr) return;
  libsrpc_req_free((libsrpc_request_t *)ptr);
}

static void libsrpc_key_init(void) {
  pthread_key_create(&libsrpc_key, libsrpc_cleanup_data);
}

/*
static void *libsrpc_get_data(void) {
  return pthread_getspecific(libsrpc_key);
} */

static void libsrpc_set_data(void *ptr) {
  if(pthread_once(&libsrpc_key_once, libsrpc_key_init) != 0) {
    ERR_PRINT("pthread_once failed\n");
    __libsrpc_errno_set(EINTR);
    return;
  }
  pthread_setspecific(libsrpc_key, ptr);
}
/* ------------------------------------------------------------------------------ */
int libsrpc_lastreq_num(void)
{
  libsrpc_request_t *req = current_req;
  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(0);
  }
  return((int)req->retnum);
}

int libsrpc_lastreq_get(int idx, void *retval, size_t sz)
{
  srpc_rc_t rc = {0};
  libsrpc_request_t  *req = current_req;
  libsrpc_response_t *resp = NULL;

  if(idx < 0) {
    return(-EINVAL);
  }

  if(libsrpc_req_is_corrupted(req, NULL) || idx >= (long)req->retnum) {
    return(-ENOTAVAILABLE);
  }

  resp = libsrpc_req_get_response_fast(req, (unsigned int)idx);
  if(! resp) {
    return(-ENOTAVAILABLE);
  }

  rc = atomic_load_explicit(&resp->rc, memory_order_acquire);
  if(rc.st == RC_ST_ERROR) {
    return(rc.code < 0 ? rc.code : -ENOTAVAILABLE);
  }
  if(rc.st != RC_ST_READY) {
    return(-ENOTAVAILABLE);
  }

  if( ALIGNLONG(sz + sizeof(libsrpc_response_t)) != req->retsz) {
    return(-EBADMSG);
  }

  if(retval && sz > 0) memcpy(retval, &resp->buf, sz);
  return(rc.code);
}

#if 0
int libsrpc_lastreq_get_proc_uid(int idx)
{
  int rc = 0;
  libsrpc_request_t  *req = current_req;
  libsrpc_response_t *resp = NULL;

  if(idx < 0) {
    return(-EINVAL);
  }

  if(libsrpc_req_is_corrupted(req, NULL) || idx >= (long)req->retnum) {
    return(-ENOTAVAILABLE);
  }

  resp = libsrpc_req_get_response_fast(req, (unsigned int)idx);
  if(! resp) {
    return(-ENOTAVAILABLE);
  }

  libsrpc_proc_t *proc = atomic_load_explicit(&resp->hp_proc, memory_order_acquire);
  if(! proc) {
    return(-ENOTAVAILABLE);
  }

  rc = atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_acquire);
  if(rc != resp->uid || ! libsrpc_proc_is_valid(proc)) return(-ENOTAVAILABLE);

  return(rc);
}
#endif

int libsrpc_shmem_proc_lock(int idx, void *ptr)
{
  uint16_t uid = 0;
  libsrpc_request_t  *req = current_req;
  libsrpc_response_t *resp = NULL;
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  libsrpc_gc_lock_t gc_lock = {0};
  int idx_lock = 0;

  if(idx < 0) {
    return(-EINVAL);
  }
  if(!shm) {
    return(-ESHMNOINIT);
  }

  if(libsrpc_req_is_corrupted(req, NULL) || idx >= (long)req->retnum) {
    return(-EINVALREQUEST);
  }

  resp = libsrpc_req_get_response_fast(req, (unsigned int)idx);
  if(! resp) {
    return(-EINVALRESPONSE);
  }

  libsrpc_proc_t *proc = atomic_load_explicit(&resp->hp_proc, memory_order_acquire);
  if(! proc) {
    return(-ENOTAVAILABLE);
  }
  uid = atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_acquire);

  /* Проверяем, что процесс не уничтожен и не произошло ABA. */
  if(uid != resp->uid || ! libsrpc_proc_is_valid(proc)) {
    return(-EPROCDESTROYED);
  }

  /* Установим блокировку на память принадлежащую процессу. */
  gc_lock = atomic_load_explicit(&req->gc_lock_uid, memory_order_relaxed);
  for(int i = 0; i < 4; i++) {
    if(gc_lock.lock[i] == 0) {
      gc_lock.lock[i] = uid;
      atomic_store_explicit(&req->gc_lock_uid, gc_lock, memory_order_release);
      idx_lock = i;
      goto check_proc;
    }
  }
  return(-ENOMEM);

check_proc:
  /* Проверяем, что процесс не уничтожен. */
  if(! libsrpc_proc_is_valid(proc) || atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_acquire) != uid) {
    gc_lock.lock[idx_lock] = 0;
    atomic_store_explicit(&req->gc_lock_uid, gc_lock, memory_order_release);
    return(-EPROCDESTROYED);
  }

  /* Проверяем, что блок памяти принадлежит процессу. */
  if(tlsf_check_uid((tlsf_t)shm->poolptr, ptr, uid) != 1) {
    gc_lock.lock[idx_lock] = 0;
    atomic_store_explicit(&req->gc_lock_uid, gc_lock, memory_order_release);
    return(-ENOTFOUND);
  }

  return(idx_lock);
}


int libsrpc_shmem_proc_unlock(int slot)
{
  libsrpc_request_t  *req = current_req;
  libsrpc_gc_lock_t gc_lock = {0};

  if(slot < 0 || slot >= 4) {
    return(-EINVAL);
  }

  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(-ENOTAVAILABLE);
  }

  gc_lock = atomic_load_explicit(&req->gc_lock_uid, memory_order_relaxed);
  gc_lock.lock[slot] = 0;
  atomic_store_explicit(&req->gc_lock_uid, gc_lock, memory_order_release);
  return(0);
}

int libsrpc_shmem_proc_all_unlock(void)
{
  libsrpc_request_t  *req = current_req;
  libsrpc_gc_lock_t gc_lock = {0};

  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(-ENOTAVAILABLE);
  }
  atomic_store_explicit(&req->gc_lock_uid, gc_lock, memory_order_release);
  return(0);
}

int libsrpc_shmem_check_uid_lock(libsrpc_request_t *req, int uid)
{
  libsrpc_gc_lock_t gc_lock = {0};

  if(uid <= 0 || uid >= 0x10000) {
    return(-EINVAL);
  }

  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(-ENOTAVAILABLE);
  }

  gc_lock = atomic_load_explicit(&req->gc_lock_uid, memory_order_relaxed);
  for(unsigned int i = 0; i < 4; i++) {
    if(gc_lock.lock[i] == (uint16_t)uid) {
      return(1);
    }
  }
  return(0);
}

static int libsrpc_shmem_is_locked_uid_proc(libsrpc_proc_t *proc, uint16_t uid)
{
  libsrpc_request_t *req = NULL;
  LIBSRPC_LIST_FOREACH(&proc->req_head, req, libsrpc_request_t, node) {
    int is_locked = libsrpc_shmem_check_uid_lock(req, uid);
    if(is_locked == 1) {
      return(1);
    }
  }
  return(0);
}

int libsrpc_shmem_is_locked_uid(uint16_t uid)
{
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  if(!shm) {
    return(0);
  }

  libsrpc_proc_t *proc = NULL;
  LIBSRPC_LIST_FOREACH(&shm->proc_head, proc, libsrpc_proc_t, proc_node) {
    if(! libsrpc_proc_is_valid(proc)) continue;
    if(libsrpc_shmem_is_locked_uid_proc(proc, uid)) {
      return(1);
    }
  }
  return(0);
}

/* ------------------------------------------------------------------------------ */
// Отправка RPC запроса.
static int libsrpc_send_request_one(libsrpc_proc_t *proc, libsrpc_request_t *req, unsigned int it)
{
  int rc = 0;
  libsrpc_response_t *resp = NULL;

  resp = libsrpc_req_get_response_fast(req, it);
  if(! resp) {
    DBG_PRINT("response is not available\n");
    libsrpc_req_dump_print(req);
    return(-ENOTAVAILABLE);
  }
  /* Отметим что hp_proc Locked на этот proc. */
  atomic_store_explicit(&resp->rc, (srpc_rc_t){.rc = 0}, memory_order_relaxed);
  resp->uid = atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_relaxed);
  /* Свой слот, до is_valid. Следующий адресат этого же req пишет в другой resp и это поле не затирает. */
  atomic_store_explicit(&resp->hp_proc, proc, memory_order_release);
  if(! libsrpc_proc_is_valid(proc)) {
    srpc_rc_t rrc = {.st = RC_ST_ERROR, .code = -EINVALPROC};
    atomic_store_explicit(&resp->rc, rrc, memory_order_relaxed);
    atomic_store_explicit(&resp->hp_proc, NULL, memory_order_release);
    DBG_PRINT("proc is not valid\n");
    return(-EINVALPROC);
  }
  if(proc->threads_run <= 0) {
    srpc_rc_t rrc = {.st = RC_ST_ERROR, .code = -ENOTAVAILABLE};
    atomic_store_explicit(&resp->rc, rrc, memory_order_relaxed);
    atomic_store_explicit(&resp->hp_proc, NULL, memory_order_release);
    DBG_PRINT("proc threads_run is not present\n");
    return(-ENOTAVAILABLE);
  }
  
  for(unsigned int y = 0; y < 16; y++) {
    for(unsigned int i = 0; i < 32; i++) {
      rc = lf_mpmc_queue_try_enqueue(&proc->queue, req);
      if(rc == LF_QUEUE_OK) break;
      libsrpc_cpu_pause(32);
    }
    if(rc == LF_QUEUE_OK) break;
    /* Если получателя вытеснили и он заблокировал очередь, подождём ... */
    //sched_yield();
    libsrpc_usleep(1 + y * 2);
  }
  if(rc != LF_QUEUE_OK) {
    srpc_rc_t rrc = {.st = RC_ST_ERROR, .code = -ENOMEM};
    atomic_store_explicit(&resp->rc, rrc, memory_order_relaxed);
    atomic_store_explicit(&resp->hp_proc, NULL, memory_order_release);
    ERR_PRINT("lf_mpmc_queue_try_enqueue failed rc=%d\n", rc);
    return(-ENOMEM);
  }

  if(proc->threads_wait > 0 && libsrpc_proc_is_valid(proc)) {
    DBG_PRINT("post sem_wakeup\n");
    libsrpc_sem_post(&proc->sem_wakeup);
  }

  return(0);
}

static void libsrpc_send_request(libsrpc_request_t *req, libsrpc_flag_send_rpc_t flags)
{
  int rc = 0;
  unsigned int retnum = 0;
  unsigned int rr_pos = 0;
  int fidx = 0;
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  srpc_regfn_shm_t *p_regfn = NULL;
  srpc_regfn_main_block_t *mb = NULL;
  srpc_regfn_ext_block_t *eb = NULL;

  if(!req || req->retnum == 0) {
    __libsrpc_errno_set(EINVAL);
    return;
  }
  if(!shm) {
    __libsrpc_errno_set(ESHMNOINIT);
    return;
  }
  if(srpc_disable_rpc_recursion) {
    __libsrpc_errno_set(ERECURSIVE);
    return;
  }
  if(!simplerpc_data->rpc_enable) {
    __libsrpc_errno_set(ERPCDISABLE);
    return;
  }

  fidx = sRPC_ID2IDX(req->funid);
  if(fidx < 0 || fidx >= sRPC_FNNUM) {
    __libsrpc_errno_set(EINVAL);
    return;
  }
  p_regfn = &shm->regfn;
  mb = &p_regfn->funcs[fidx].main_block;
  eb = atomic_load_explicit(&p_regfn->funcs[fidx].ext_block, memory_order_acquire);

  retnum = req->retnum;
  if(flags == RPC_SEND_FIRST) {
    retnum = 1;
  }
  if(flags == RPC_SEND_RR) {
    rr_pos = atomic_fetch_add(&p_regfn->req_send_rr[fidx], 1);
    rr_pos = rr_pos % (retnum > 1 ? retnum : 1);
    retnum = 1;
  }
  req->retnum = retnum;

  libsrpc_proc_t *proc = NULL;
  libsrpc_proc_t *proc_last = NULL;
  unsigned int it = 0;
  unsigned int rr_cnt = 0;
DBG_PRINT("send request to %d workers\n", retnum);
  // Отправляем запросы в очереди всех (но не более retnum) зарегистрированных процессов, которые имеют запущенные потоки.
  for(unsigned int i = 0; i < LIBSRPC_RFMB_SZ && it < retnum; i++) {
    proc = atomic_load(&mb->proc[i]);
    if(proc != NULL) {
      if(flags == RPC_SEND_LAST) { proc_last = proc; continue; }
      if(flags == RPC_SEND_RR && rr_pos != rr_cnt++) continue;
      rc = libsrpc_send_request_one(proc, req, it);
      if(rc == 0) it++;
    }
  }

  if(eb && it < retnum) {
    do{
      atomic_store_explicit(&req->hp_regfn, eb, memory_order_release); // Lock HP_REGFN.
      srpc_regfn_ext_block_t *eb_tmp = atomic_load_explicit(&p_regfn->funcs[fidx].ext_block, memory_order_acquire);
      if(eb_tmp == eb) break;
      eb = eb_tmp;
    }while(1);
    for(unsigned int i = 0; i < eb->szfn && it < retnum; i++) {
      proc = atomic_load(&eb->proc[i]);
      if(proc != NULL) {
        if(flags == RPC_SEND_LAST) { proc_last = proc; continue; }
        if(flags == RPC_SEND_RR && rr_pos != rr_cnt++) continue;
        rc = libsrpc_send_request_one(proc, req, it);
        if(rc == 0) it++;
      }
    }
    atomic_store_explicit(&req->hp_regfn, NULL, memory_order_release); // Unlock HP_REGFN.
  }

  if(flags == RPC_SEND_LAST && proc_last != NULL) {
    rc = libsrpc_send_request_one(proc_last, req, it);
    if(rc == 0) it++;
    retnum = 1;
  }

  if(it == 0) { /* не удалось отправить ни один запрос */
    __libsrpc_errno_set(ENOTAVAILABLE);
    ERR_PRINT("no requests sent\n");
    return;
  }
  if(it != retnum) { /* не удалось отправить все запросы */
    ERR_PRINT("some requests not sent: retnum=%u, it=%u\n", retnum, it);
    retnum = it;
  }
  req->retnum = retnum;

  /* Определяем таймаут ожидания ответов от исполнителей. */
  uint64_t timeout = rpc_timeout_global;
  if(rpc_timeout_oneshot > 0) {
    timeout = rpc_timeout_oneshot;
    rpc_timeout_oneshot = 0;
  }else{
    uint64_t timeout_func = rpc_timeout[fidx];
    if(timeout_func > 0) {
      timeout = timeout_func;
    }
  }

  if(timeout < LIBSRPC_TIMEOUT_MINIMUM) {
    timeout = LIBSRPC_TIMEOUT_MINIMUM;
  }

  struct timespec ts = {0};
  libsrpc_sem_time_calc(&ts, timeout);

  DBG_PRINT("wait for responses from customers\n");
  /* Ждём обработки всех запросов исполнителями. */
  unsigned int cnt_rcv;
  do{
    cnt_rcv = 0;
    rc = libsrpc_sem_timedwait(&req->sem_wakeup, &ts);
    if(rc < 0) {
      int err = errno;
      if(err == ETIMEDOUT || err == EINVAL) {
        /* Промаркируем пустые ответы как ошибки таймаута */
        for(unsigned int i = 0; i < retnum; i++) {
          srpc_rc_t expected = {0};
          srpc_rc_t rrc = {.st = RC_ST_ERROR, .code = -ERPCWAITIMEDOUT};
          libsrpc_response_t *resp = libsrpc_req_get_response_fast(req, i);
          if(! resp) continue;
          atomic_compare_exchange_strong_explicit(&resp->rc, &expected, rrc, memory_order_acquire, memory_order_relaxed);
        }
        goto end;
      }
    }

    /* Подсчитаем полученные ответы. */
    for(unsigned int i = 0; i < retnum; i++) {
      libsrpc_response_t *resp = libsrpc_req_get_response_fast(req, i);
      if(! resp) continue;
      srpc_rc_t rrc = atomic_load_explicit(&resp->rc, memory_order_acquire);
      if(atomic_load_explicit(&resp->hp_proc, memory_order_acquire) != NULL &&
        (rrc.rc == 0 || rrc.st == RC_ST_LOCK)) continue;
      cnt_rcv++;
    }

  }while(cnt_rcv < retnum);

end:
  DBG_PRINT("wait for client done\n");
  return;
}


int libsrpc_req_is_busy_proc(libsrpc_request_t *req)
{
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  if(!req || !shm) {
    return(0);
  }

  libsrpc_proc_t *proc = NULL;
  LIBSRPC_LIST_FOREACH(&shm->proc_head, proc, libsrpc_proc_t, proc_node) {
    if(! libsrpc_proc_is_valid(proc)) continue;

    /* Проверка запроса в очереди процесса */
    lf_queue_result_t result = lf_mpmc_queue_try_find_ptr(&proc->queue, req);
    if(result == LF_QUEUE_OK) {
      return(1);
    }

    /* Проверка запросов */
    for(unsigned int i = 0; i < proc->threads_num; i++) {
        void *hp = atomic_load_explicit(&proc->threads[i].hp_req, memory_order_acquire);
        if(hp != req) continue; /* Поток не блокирует запрос */
        return(1); /* Запрос заблокирован, сообщим вызывающему */
    }
  }
  return(0);
}

#if 0
/* TODO: переделать на проверку по всем живым процессам, и убрать проверку corrupted и в req не заходить */
int libsrpc_req_is_busy(libsrpc_request_t *req)
{
  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(0);
  }

  for(unsigned int i = 0; i < req->retnum; i++) {
    libsrpc_response_t *resp = libsrpc_req_get_response(req, i);
    if(! resp) continue; /* Запрос некорректный */
    srpc_rc_t rrc = atomic_load_explicit(&resp->rc, memory_order_acquire);
    if(rrc.rc != 0) continue; /* Уже ответил */
    if(! libsrpc_proc_is_valid(resp->hp_proc)) continue; /* Процесс невалидный */
    for(unsigned int j = 0; j < resp->hp_proc->threads_num; j++) {
      libsrpc_proc_thread_t *thread = &resp->hp_proc->threads[j];
      if(thread->hp_req != req) continue; /* Поток не блокирует запрос */
      return(1); /* Запрос заблокирован, сообщим вызывающему */
    }
  }

  return(0);
}
#endif


static int libsrpc_req_is_bad(libsrpc_request_t *req)
{
  if(!req) {
    return(1);
  }
  //if(libsrpc_req_is_corrupted(req, NULL)) {
  //  return(1);
  //}
  for(unsigned i = 0; i < req->retnum; i++) {
    libsrpc_response_t *resp = libsrpc_req_get_response_fast(req, i);
    if(! resp) return(1);
    srpc_rc_t rc = atomic_load_explicit(&resp->rc, memory_order_acquire);
    if(rc.st == RC_ST_LOCK || rc.st == RC_ST_ERROR ||
       rc.rc == -ERPCWAITIMEDOUT ||
       rc.rc == 0
      ) return(1);
  }

  return(0);
}

static void libsrpc_req_free(libsrpc_request_t *req)
{
  if(!req) {
    return;
  }
  libsrpc_list_remove(&req->node);
  libsrpc_shmem_free_dc(req);
  libsrpc_shm_gc_wakeup();
}

static _Atomic(uint32_t) libsrpc_req_seq_num = 0;

static libsrpc_request_t* libsrpc_req_alloc(int funid, unsigned int reqlen, unsigned int retsz)
{
  libsrpc_request_t *req = NULL;
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  libsrpc_proc_t *proc = simplerpc_data->proc;
  size_t sz = 0;
  unsigned int retnum = 0;
  unsigned int retoff = 0;

  if(!shm) {
    __libsrpc_errno_set(ESHMNOINIT);
    ERR_PRINT("shm is NULL\n");
    return(NULL);
  }

  srpc_regfn_shm_t *p_regfn = &shm->regfn;
  retnum = (size_t)atomic_load(&p_regfn->funcs[sRPC_ID2IDX(funid)].num_all);
  if(retnum == 0) {
    __libsrpc_errno_set(ENOREGFUN);
    ERR_PRINT("The function '%s' is not linked\n", simplerpc_data->fn[sRPC_ID2IDX(funid)].name);
    return(NULL);
  }

  retoff = ALIGNLONG(reqlen);
  retsz  = ALIGNLONG(sizeof(libsrpc_response_t) + retsz);
  sz     = sizeof(libsrpc_request_t) + (size_t)retoff + ((size_t)retsz * (size_t)retnum);

  int is_bad = libsrpc_req_is_bad(current_req);
  if(! is_bad &&
     current_req->bufsz >= sz
    ) { // Если помещается в текущий блок запроса и не завис, то используем его.
    req = current_req;
  }else{ // Если не помещается или не существует, то освобождаем текущий блок запроса и аллоцируем новый.
    libsrpc_request_t *req_old = current_req;

    sz *= 2; // с запасом, следующие запросы могут быть больше текущего, снизим расходы на аллокацию.
    req = libsrpc_shmem_malloc_type(sz, LIBSRPC_SHMDT_REQUEST);
    if(req == NULL) {
      __libsrpc_errno_set(ENOMEM);
      ERR_PRINT("failed to allocate memory\n");
      return(NULL);
    }
    current_req = req;
    memset(req, 0, sz);
    req->sign = LIBSRPC_REQ_SIGN;
    if(req_old) req->gc_lock_uid = atomic_load_explicit(&req_old->gc_lock_uid, memory_order_relaxed);
    libsrpc_sem_init(&req->sem_wakeup, 0);
    libsrpc_list_node_init(&req->node);
    libsrpc_list_push_front(&proc->req_head, &req->node);
    libsrpc_set_data(req);
    if(req_old) {
      libsrpc_req_free(req_old);
    }
  }

  req->seq_num = atomic_fetch_add(&libsrpc_req_seq_num, 1);
  req->funid  = funid;
  req->bufsz  = (unsigned int)sz;
  req->retoff = retoff;
  req->retsz  = retsz;
  req->retnum = retnum;

  return(req);
}

/* ------------------------------------------------------------------------------ */
// Реализации функций, перехватывающих обёртки, для RPC вызовов.
#define XF(flags,rettype,name,...) \
static rettype sRPCFN(name)(M_ARGFUN(__VA_ARGS__)) { \
    unsigned int len=0; unsigned int rlen=0; unsigned int pos=0; \
    libsrpc_request_t *req = NULL; \
    __libsrpc_errno_clear(); \
    rlen = RLEN(rettype); \
    if(rlen > 32768) abort(); \
    char rbuf[rlen > 0 ? rlen : 1]; \
    memset(rbuf, 0, rlen); \
    len = M_REQSIZE(__VA_ARGS__) 0; \
    req = libsrpc_req_alloc(GET_FNID(name), len, rlen); \
    if(req != NULL) { \
      M_BODYFUN(__VA_ARGS__); \
      if(!!req && pos != len) { __libsrpc_errno_set(EBADMSG); }else{ \
        libsrpc_response_t *resp = libsrpc_req_get_response_fast(req, 0); \
        libsrpc_send_request(req, flags); \
        int grc = libsrpc_req_response_get(resp, rbuf, rlen); \
        if(grc < 0) __libsrpc_errno_set(-grc); \
      } \
    } \
    DBG_PRINT("RPC call: %s %s(%s fnid=%d) len=%d retsz=%d\n", #rettype, #name, #__VA_ARGS__, GET_FNID(name), len, rlen); \
    RETDATA(rettype,rbuf); \
  }
RPC_LIST
#undef XF

// Прототипы ф-ий с атрибутом weak, для link-овки. Если функция определена в текущей приложении, будет вызвана она, иначе подмена.
#define XF(flags,rettype,name,...) \
  __attribute__((weak, alias("librpcimp_" #name))) \
  rettype name(__VA_ARGS__);
RPC_LIST
#undef XF

// Указатели на функции, оригинал (если есть в текущем приложении) и подмену.
static const srpc_func_t srpc_fn[sRPC_FNNUM] = {
#define XF(flags,rettype,fname,...)  {.rpc = sRPCFN(fname), .loc = fname, .name = #fname },
RPC_LIST
#undef XF
};

static simplerpc_t srpc = {
  .sign = {BUILD_TS},
  .fn = srpc_fn,
  .fn_sz = sRPC_FNNUM,
};

/* ============================================================================== */
/* --- Макросы для десериализации и вызова (Callback) --- */

// 1. Объявление переменных: int p1, char* p2, ...
//#define M_DECL(t,v,f) t v COMMA_IF(f)
#define M_DECL(t,v,f) t v;
#define M_DECLFUN(...) EVAL(FOREACH2(M_DECL,p,,__VA_ARGS__))

// 2. Извлечение из буфера: memcpy(&p1, &buf[pos], sizeof(p1)); pos += sizeof(p1); ...
#define M_EXTRACT(t,v,f) memcpy(&v, &req->buf[pos], sizeof(v)); pos += sizeof(v);
#define M_EXTRACTFUN(...) EVAL(FOREACH2(M_EXTRACT,p,,__VA_ARGS__))

// 3. Генерация списка аргументов для вызова: p1, p2, ...
#define M_ARGNAME(t,v,f) v COMMA_IF(f)
#define M_ARGNAMES(...) EVAL(FOREACH2(M_ARGNAME,p,,__VA_ARGS__))

/* RPC Callbacks */
static int libsrpc_callback_func(libsrpc_request_t *req, libsrpc_response_t *resp, libsrpc_req_ctrl_t *ctrl)
{
  int rc = 0;

  if( !req || !resp) return(-ENOTAVAILABLE);

  switch(req->funid) {
    #define XF(flags,rettype,name,...) \
        case GET_FNID(name): \
            /* Защита от рекурсии: если зарегистрирована сама обертка */ \
            if ((void*)&(name) == (void*)&(sRPCFN(name))) { rc = -ERECURSIVE; break; } \
            { \
                unsigned int pos = 0; \
                /* Объявляем переменные (int p, char* pp, ...) */ \
                M_DECLFUN(__VA_ARGS__); \
                /* Копируем данные из buf в переменные */ \
                M_EXTRACTFUN(__VA_ARGS__); \
                \
                /* Вызываем функцию и обрабатываем возвращаемое значение */ \
                IIF(EQUAL(rettype, void)) \
                ( \
                    /* Если void: просто вызываем */ \
                    name(M_ARGNAMES(__VA_ARGS__)); \
                    rc = libsrpc_req_response_set(req, resp, ctrl, NULL, 0); \
                , \
                    /* Если не void: сохраняем результат и пишем в буфер ответов */ \
                    rettype retval = name(M_ARGNAMES(__VA_ARGS__)); \
                    rc = libsrpc_req_response_set(req, resp, ctrl, &retval, sizeof(retval)); \
                ) \
                (void)pos; \
            } \
            break;
        RPC_LIST
    #undef XF
    default:
      ERR_PRINT("srpc_callback_func: BAD FUNID: funid=%d\n", req->funid);
      rc = -EBADMSG;
      break;
  } // switch(req->funid)
  libsrpc_sem_post(&req->sem_wakeup);
  return(rc);
}
/* ------------------------------------------------------------------------------ */
// Поток приема и обработки запросов.
static atomic_char srpc_thread_stop_flag = 0;

static int libsrpc_request_lock(libsrpc_proc_thread_t *thread, libsrpc_request_t *req)
{
  if(!thread || !req) {
    return -EINVAL;
  }

  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(-EBADMSG);
  }

  atomic_store_explicit(&thread->hp_req, (void*)req, memory_order_release);

  if(libsrpc_req_is_corrupted(req, NULL)) {
    atomic_store_explicit(&thread->hp_req, NULL, memory_order_release);
    return(-EBADMSG);
  }
  return(0);
}

static int libsrpc_request_unlock(libsrpc_proc_thread_t *thread)
{
  int rc = 0;
  if(!thread) {
    return -EINVAL;
  }
  atomic_store_explicit(&thread->hp_req, NULL, memory_order_release);
  return(rc);
}

static int libsrpc_request_processing(libsrpc_request_t *req, libsrpc_proc_t *proc, libsrpc_proc_thread_t *thread)
{
  int rc = 0;

  if(! req || ! proc || ! thread) {
    return(-EINVAL);
  }

  libsrpc_request_lock(thread, req);

  if(libsrpc_req_is_corrupted(req, NULL)) {
    libsrpc_request_unlock(thread);
    ERR_PRINT("corrupted request: req=%p\n", (void*)req);
    return(-EBADMSG);
  }
  libsrpc_req_ctrl_t ctrl = atomic_load_explicit(&req->control, memory_order_acquire);

  if(req->funid >= sRPCFNID_MAX || req->funid <= sRPCFNID_START) {
    libsrpc_request_unlock(thread);
    ERR_PRINT("BAD REQUEST: req=%p funid=%d\n", (void*)req, req->funid);
    return(-EBADMSG);
  }

  /* Найдём свой слот для ответа на запрос. */
  libsrpc_response_t *resp = NULL;
  for(unsigned int i = 0; i < req->retnum; i++) {
    resp = libsrpc_req_get_response_fast(req, i);
    if(! resp) continue;
    if(atomic_load_explicit(&resp->hp_proc, memory_order_acquire) != proc) continue;
    if(atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_acquire) != resp->uid) continue;
    break;
  }
  if(! resp || resp->hp_proc != proc || resp->uid != proc->proc_uid) {
    libsrpc_request_unlock(thread);
    ERR_PRINT("BAD RESPONSE: req=%p resp=%p\n", (void*)req, (void*)resp);
    return(-EBADMSG);
  }

  INF_PRINT("dequeue success[%d]: req=%p funid=%d retsz=%d\n", sched_getcpu(), (void*)req, req->funid, req->retsz);

  rc = libsrpc_callback_func(req, resp, &ctrl);
  if(((srpc_rc_t){.rc = rc}).st != RC_ST_READY) {
    libsrpc_request_unlock(thread);
    WRN_PRINT("callback function failed: rc=%d\n", rc);
    return(-EBADMSG);
  }

  libsrpc_request_unlock(thread);

  return(0);
}

// Функция потока приема и обработки запросов.
static void * libsrpc_thread_rcv_req(void *arg __attribute__((unused)))
{
  libsrpc_proc_thread_t *thread = NULL;
  libsrpc_proc_t *proc = simplerpc_data->proc;
  libsrpc_request_t *req = NULL;
  int rc = 0;
  unsigned int tid = 0;
  int cnt = 0;
  bool wait_flag = false;

  srpc_disable_rpc_recursion = 1;

  DBG_PRINT("started CPU=%d\n", sched_getcpu());

  tid = atomic_fetch_add(&proc->threads_run, 1);
  if(tid >= proc->threads_num) {
    atomic_fetch_sub(&proc->threads_run, 1);
    ERR_PRINT("threads_run overflow: tid=%d threads_num=%d\n", tid, proc->threads_num);
    return(NULL);
  }
  thread = &proc->threads[tid];
  thread->tid = pthread_self();

#define DEQUEUE_RETRY 1024
  while(1) {
    if(!! atomic_load_explicit(&srpc_thread_stop_flag, memory_order_acquire)) break;

    while(lf_mpmc_queue_try_dequeue(&proc->queue, (void**)&req) == LF_QUEUE_OK && !! req) {
      if(wait_flag) {
        atomic_fetch_sub(&proc->threads_wait, 1);
        wait_flag = false;
      }
      rc = libsrpc_request_processing(req, proc, thread);
      if(rc < 0 && ((srpc_rc_t){.rc = rc}).st != RC_ST_READY) {
        ERR_PRINT("request processing failed: rc=%d\n", rc);
      }
      cnt = 0;
      req = NULL;
    } // while dequeue
    if(cnt++ < DEQUEUE_RETRY) {
      libsrpc_cpu_pause(32);
    }else{
      if(! wait_flag) {
        wait_flag = true;
        atomic_fetch_add(&proc->threads_wait, 1);
      }else{
        rc = libsrpc_sem_wait(&proc->sem_wakeup);
        atomic_fetch_sub(&proc->threads_wait, 1);
        wait_flag = false;
        if(unlikely(rc == -1 && errno == EINVAL)) {
          ERR_PRINT("sem_wait failed: %s\n", strerror(errno));
          break;
        }
      }
    }// else cnt
  } // while(1)

  atomic_fetch_sub(&proc->threads_run, 1);

  DBG_PRINT("stopped CPU=%d\n", sched_getcpu());
  return(NULL);
}

int libsrpc_thread_rcv_req_start(void) {
  int rc = 0;
  rc = pthread_start_all_cpu(libsrpc_thread_rcv_req, NULL);
  if (rc != 0) {
    ERR_PRINT("pthread_start_all_cpu failed\n");
    libsrpc_thread_rcv_req_stop();
    goto err;
  }
  DBG_PRINT("started all CPU threads\n");
out:
  return(rc);
err:
  goto out;
}

// Очистка очереди запросов неактивного процесса.
void libsrpc_thread_rcv_queue_clear(libsrpc_proc_t *proc)
{
  libsrpc_request_t *req = NULL;

  /* освободим зависшие RPC запросы в очереди */
  while(lf_mpmc_queue_try_dequeue(&proc->queue, (void**)&req) == LF_QUEUE_OK) {
    if(libsrpc_req_is_corrupted(req, NULL)) continue;
    /* Найдём свой слот для ответа на запрос. */
    libsrpc_response_t *resp = NULL;
    for(unsigned int i = 0; i < req->retnum; i++) {
      resp = libsrpc_req_get_response(req, i);
      if(! resp) continue;
      if(atomic_load_explicit(&resp->hp_proc, memory_order_acquire) != proc) continue;
      if(atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_acquire) != resp->uid) continue;
      break;
    }
    if(! resp || atomic_load_explicit(&resp->hp_proc, memory_order_acquire) != proc || atomic_load_explicit((_Atomic(uint16_t)*)&proc->proc_uid, memory_order_acquire) != resp->uid) continue;
    /* Запрос не был выполнен, установим RC код ошибки. */
    srpc_rc_t expected = {0};
    srpc_rc_t rrc = {.st = RC_ST_ERROR, .code = -ECANCELLED};
    atomic_compare_exchange_strong_explicit(&resp->rc, &expected, rrc, memory_order_acquire, memory_order_relaxed);
    if(atomic_load_explicit(&req->sign, memory_order_acquire) == LIBSRPC_REQ_SIGN) { // <= ИИ говорит без того будет UB, добавли чтоб он отстал :)
      libsrpc_sem_post(&req->sem_wakeup);
    }
  }
}

int libsrpc_thread_rcv_req_stop(void) {
  int rc = 0;
  libsrpc_proc_t *proc = simplerpc_data->proc;
  unsigned int n = atomic_load(&proc->threads_wait) + 1;

  atomic_store(&srpc_thread_stop_flag, 1);

  for(unsigned int i = 0; i < n; i++) {
    libsrpc_sem_post(&proc->sem_wakeup);
  }

  // Очищаем очередь запросов.
  libsrpc_thread_rcv_queue_clear(proc);

  // Ждём пока все потоки завершатся.
  while(atomic_load_explicit(&proc->threads_run, memory_order_acquire) > 0) {
    libsrpc_usleep(100);
  }

  DBG_PRINT("send stopped\n");
  return(rc);
}

/* ============================================================================== */
// Регистрация своих функций в RPC.

int libsrpc_fnreg_num_get(libsrpc_funid_t funid)
{
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  if(!shm) {
    return(-ESHMNOINIT);
  }
  if(funid >= LIBSRPC_FUNID_MAX || funid <= LIBSRPC_FUNID_START) {
    return(-EINVAL);
  }
  return((int)atomic_load(&shm->regfn.funcs[sRPC_ID2IDX(funid)].num_all));
}

void libsrpc_bmp_func_set(srpc_bmp_func_t *bmp)
{
  if(!bmp) {
    return;
  }

  for(int i = 0; i < sRPC_FNNUM; i++) {
    if(srpc_fn[i].loc != srpc_fn[i].rpc) {
      bmp->bmp[i / 64] |= (1ULL << (i % 64));
    }
  }
}

/* Функция вызывается только одинм потоком демона, иначе может быть гонка данных */
static int libsrpc_regfn_ext_block(libsrpc_rpc_func_t *func)
{
  size_t     sz = sizeof(srpc_regfn_ext_block_t) + (LIBSRPC_RFEB_SZ * sizeof(libsrpc_proc_t *));
  uint32_t szfn = LIBSRPC_RFEB_SZ;
  srpc_regfn_ext_block_t *old_eb = func->ext_block;
  srpc_regfn_ext_block_t *eb = NULL;

  if(old_eb) {
    szfn += old_eb->szfn;
    sz = old_eb->size + (szfn * sizeof(libsrpc_proc_t *));
  }

  eb = libsrpc_shmem_malloc_type(sz, LIBSRPC_SHMDT_REGFN);
  if(!eb) return(-ENOMEM);

  if(old_eb) {
    size_t szmc = old_eb->size;
    if(szmc > sz) {
      ERR_PRINT("szmc > sz: szmc=%zu sz=%zu\n", szmc, sz);
      szmc = sz;
    }
    memcpy(eb, old_eb, szmc);
  }

  eb->sign = LIBSRPC_REGFN_SIGN;
  eb->szfn = szfn;
  eb->size = sz;

  if(!atomic_compare_exchange_strong(&func->ext_block, &old_eb, eb)){
    libsrpc_shmem_free(eb);
    return(-ENOMEM);
  }

  if(old_eb) {
    atomic_store_explicit(&old_eb->sign, 0, memory_order_release);
    libsrpc_shmem_free_dc(old_eb); // Отложенное удаление блока, GC проверит занятость и удалит если не используется.
    libsrpc_shm_gc_wakeup(); // Пробудим GC.
  }

  return(0);
}

static int libsrpc_regfn_insert(libsrpc_proc_t *proc, libsrpc_rpc_func_t *func, srpc_regfn_main_block_t *mb, srpc_regfn_ext_block_t *eb)
{
  int rc = -ENOMEM;

  /* Попробуем вставить в главный блок */
  for(uint32_t i = 0; i < LIBSRPC_RFMB_SZ; i++) {
    if(atomic_load(&mb->proc[i]) == NULL) {
      libsrpc_proc_t *old_proc = NULL;
      if(atomic_compare_exchange_strong(&mb->proc[i], &old_proc, proc)) {
        atomic_fetch_add(&func->num_all, 1);
        return(0);
      }
    }
  }

  for(int j = 0; j < 10; j++) {
    eb = atomic_load(&func->ext_block);
    if(!eb || atomic_load(&func->num_all) >= eb->szfn) {
      rc = libsrpc_regfn_ext_block(func);
      if(rc < 0) continue;
      eb = atomic_load(&func->ext_block);
      if(!eb) continue;
    }

    rc = -ENOMEM;
    /* Попробуем вставить в расширенный блок */
    for(uint32_t i = 0; i < eb->szfn; i++) {
      if(atomic_load(&eb->proc[i]) == NULL) {
        libsrpc_proc_t *old_proc = NULL;
        if(atomic_compare_exchange_strong(&eb->proc[i], &old_proc, proc)) {
          atomic_fetch_add(&func->num_all, 1);
          return(0);
        }
      }
    }
  }

  return(rc);
}

int libsrpc_reg_func_form_bmp(libsrpc_proc_t *proc, srpc_bmp_func_t *bmp)
{
  int rc = 0;
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  if(!shm) {
    return(-ESHMNOINIT);
  }
  srpc_regfn_shm_t *p_regfn = &shm->regfn;
  srpc_regfn_main_block_t *mb = NULL;
  srpc_regfn_ext_block_t *eb = NULL;

  for(int i = 0; i < sRPC_FNNUM; i++) {
    if((bmp->bmp[i / 64] & (1ULL << (i % 64))) != 0) {
      mb = &p_regfn->funcs[i].main_block;
      eb = p_regfn->funcs[i].ext_block;
      rc = libsrpc_regfn_insert(proc, &p_regfn->funcs[i], mb, eb);
      if(rc < 0) {
        ERR_PRINT("insert '%s' failed\n", srpc_fn[i].name);
      }
    }
  }
  return(rc);
}

static int libsrpc_regfn_remove(libsrpc_proc_t *proc, libsrpc_rpc_func_t *func, srpc_regfn_main_block_t *mb, srpc_regfn_ext_block_t *eb)
{
  int rc = 0;

  /* Удалим из главного блока */
  for(uint32_t i = 0; i < LIBSRPC_RFMB_SZ; i++) {
    if(atomic_load(&mb->proc[i]) == proc) {
      atomic_store_explicit(&mb->proc[i], NULL, memory_order_release);
      atomic_fetch_sub(&func->num_all, 1);
    }
  }

  if(!eb) return(0);

  /*  Удалим из расширенного блока */
  for(uint32_t i = 0; i < eb->szfn; i++) {
    if(atomic_load(&eb->proc[i]) == proc) {
      atomic_store_explicit(&eb->proc[i], NULL, memory_order_release);
      atomic_fetch_sub(&func->num_all, 1);
    }
  }

  return(rc);
}

int libsrpc_unreg_func_proc(libsrpc_proc_t *proc)
{
  int rc = 0;

  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  if(!shm) {
    return(-ESHMNOINIT);
  }
  srpc_regfn_shm_t *p_regfn = &shm->regfn;
  srpc_regfn_main_block_t *mb = NULL;
  srpc_regfn_ext_block_t *eb = NULL;

  for(int i = 0; i < sRPC_FNNUM; i++) {
    mb = &p_regfn->funcs[i].main_block;
    eb = p_regfn->funcs[i].ext_block;
    rc = libsrpc_regfn_remove(proc, &p_regfn->funcs[i], mb, eb);
    if(rc < 0) {
      ERR_PRINT("remove '%s' failed\n", srpc_fn[i].name);
    }
  }
  return(rc);
}

static int libsrpc_shmem_is_locked_regfn_proc(libsrpc_proc_t *proc, srpc_regfn_ext_block_t *eb)
{
  libsrpc_request_t *req = NULL;
  if(!proc || !eb) return(0);

  LIBSRPC_LIST_FOREACH(&proc->req_head, req, libsrpc_request_t, node) {
    if(libsrpc_req_is_corrupted(req, NULL)) continue;
    if(atomic_load_explicit(&req->hp_regfn, memory_order_acquire) == eb) {
      return(1);
    }
  }
  return(0);
}

int libsrpc_shmem_is_busy_regfn(srpc_regfn_ext_block_t *eb)
{
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  libsrpc_proc_t *proc = NULL;
  if(!eb || !shm) return(0);
  LIBSRPC_LIST_FOREACH(&shm->proc_head, proc, libsrpc_proc_t, proc_node) {
    if(! libsrpc_proc_is_valid(proc)) continue;
    if(libsrpc_shmem_is_locked_regfn_proc(proc, eb) == 1) {
      return(1);
    }
  }
  return(0);
}

/* ============================================================================== */
/* SHARED MEMORY POLL and allocator */

enum {
  SRPC_ALLOC_STD = 0,
  SRPC_ALLOC_SHM,
};

static atomic_char en_allocator = 0; // fucking TLS(thread local storage) |==:=>-.

#ifndef LIBSRPC_DISABLE_SUBSTITUTION_ALLOCATOR
_Thread_local static atomic_char switch_allocator = SRPC_ALLOC_STD;
void libsrpc_alloc_sw_std(void) {
  switch_allocator = SRPC_ALLOC_STD;
}

void libsrpc_alloc_sw_shm(void) {
  switch_allocator = SRPC_ALLOC_SHM;
}

// Переопределение malloc
__attribute__((malloc))
void* malloc(size_t size) {
  void* ptr = NULL;

  if(!en_allocator) {
    if(srpc.real_malloc) ptr = srpc.real_malloc(size);
    return ptr;
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      ptr = libsrpc_shmem_malloc(size);
      break;
    default:
      // Используем реальный malloc
      if(srpc.real_malloc) ptr = srpc.real_malloc(size);
      break;
  }

    return ptr;
}

void* calloc(size_t num, size_t size)
{
  void* ptr = NULL;

  if(!en_allocator) {
    if(srpc.real_calloc) ptr = srpc.real_calloc(num, size);
    return ptr;
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      ptr = libsrpc_shmem_calloc(num, size);
      break;
    default:
      if(srpc.real_calloc) ptr = srpc.real_calloc(num, size);
      break;
  }

  return(ptr);
}

void* realloc(void* oldptr, size_t newsize)
{
  void* ptr = NULL;

  if(!en_allocator) {
    if(srpc.real_realloc) ptr = srpc.real_realloc(oldptr, newsize);
    return(ptr);
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      ptr = libsrpc_shmem_realloc(oldptr, newsize);
      break;
    default:
      if(srpc.real_realloc) ptr = srpc.real_realloc(oldptr, newsize);
      break;
  }

  return(ptr);
}

// Переопределение free
void free(void* ptr) {

  if(!en_allocator) {
    if(srpc.real_free) srpc.real_free(ptr);
    return;
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      libsrpc_shmem_free(ptr);
      break;
    default:
      // Используем реальный free
      if(srpc.real_free) srpc.real_free(ptr);
      break;
  }
}
#endif
/* ============================================================================== */
extern char *program_invocation_name;

__attribute__((constructor(101)))
static void libsrpc_init() {
    // Получаем адрес настоящих функций malloc и free
    srpc.real_malloc  = (void* (*)(size_t)) dlsym(RTLD_NEXT, "malloc");
    srpc.real_calloc  = (void* (*)(size_t, size_t)) dlsym(RTLD_NEXT, "calloc");
    srpc.real_realloc = (void* (*)(void*, size_t)) dlsym(RTLD_NEXT, "realloc");
    srpc.real_free    = (void (*)(void*)) dlsym(RTLD_NEXT, "free");
#ifndef LIBSRPC_DISABLE_SUBSTITUTION_ALLOCATOR
    if (!srpc.real_malloc || !srpc.real_free || !srpc.real_calloc || !srpc.real_realloc) {
        const char *error_msg = "Error loading malloc/free: dlsym failed.\n";
        (void)write(STDERR_FILENO, error_msg, strlen(error_msg));
        exit(1);  // Завершаем программу, если не удалось загрузить настоящие функции
    }
#endif
    //DBG_PRINT("malloc %p:%p calloc %p:%p realloc %p:%p free %p:%p\n", srpc.real_malloc, malloc, srpc.real_calloc, calloc, srpc.real_realloc, realloc, srpc.real_free, free);

    DBG_PRINT("Программа %s PID=%d скомпилирована: %s\n", program_invocation_name, getpid(), BUILD_TS );
    DBG_PRINT("daemon_name = %s  program_invocation_name = %s\n", libsrpc_daemon_name, program_invocation_name);

    if(!!strcmp(program_invocation_name, libsrpc_daemon_name)) {
      DBG_PRINT("spawn_daemon\n");
      spawn_daemon(libsrpc_daemon_name);
      en_allocator = 1;
      libsrpc_unix_client_init(&simplerpc_data->cli, libsrpc_daemon_name);
    }else{
      DBG_PRINT("daemon program\n");
    }
}

__attribute__((destructor))
static void libsrpc_destructor(void) {
    DBG_PRINT("Библиотека выгружена: деструктор вызван PID=%d\n", getpid());
    if(simplerpc_data->is_daemon) {
      libsrpc_unix_server_exit(&simplerpc_data->srv);
    } else {
      libsrpc_unix_client_exit(&simplerpc_data->cli);
    }
}
/* ============================================================================== */


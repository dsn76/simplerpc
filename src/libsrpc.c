
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

void libsrpc_req_dump_print(libsrpc_request_t *req __attribute__((unused)))
{
  DBG_PRINT("req: %p sign: %X funid: %d bufsz: %d retoff: %d retsz: %d retnum: %d\n", req, req->sign, req->funid, req->bufsz, req->retoff, req->retsz, req->retnum);
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
int libsrpc_lastreq_num(void)
{
  libsrpc_request_t *req = current_req;
  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(0);
  }
  return(req->retnum);
}

int libsrpc_lastreq_get(int idx, void *retval, size_t sz)
{
  srpc_rc_t rc = 0;
  libsrpc_request_t  *req = current_req;
  libsrpc_response_t *resp = NULL;

  if(idx < 0) {
    return(-EINVAL);
  }

  if(libsrpc_req_is_corrupted(req, NULL) || idx >= (long)req->retnum) {
    return(-ENOTAVAILABLE);
  }

  resp = libsrpc_req_get_response(req, idx);
  if(! resp) {
    return(-ENOTAVAILABLE);
  }

  if( ALIGNLONG(sz + sizeof(libsrpc_response_t)) != req->retsz) {
    return(-EBADMSG);
  }

  memcpy(retval, &resp->buf, sz);
  memcpy(&rc, &resp->rc, sizeof(srpc_rc_t));
  if((unsigned)rc == LIBSRPC_RC_FLAG_READY) rc = 0; /* Маскировка флага готовности ответа */
  return(rc);
}

/* ------------------------------------------------------------------------------ */
// Отправка RPC запроса.
static int libsrpc_send_request_one(libsrpc_proc_t *proc, libsrpc_request_t *req, unsigned int it)
{
  int rc = 0;
  libsrpc_response_t *resp = NULL;
  if(! libsrpc_proc_is_valid(proc)) {
    DBG_PRINT("proc is not valid\n");
    return(-EINVAL);
  }
  if(proc->threads_run <= 0) {
    DBG_PRINT("proc threads_run is not present\n");
    return(-ENOTAVAILABLE);
  }
  resp = libsrpc_req_get_response(req, it);
  if(! resp) {
    DBG_PRINT("response is not available\n");
    libsrpc_req_dump_print(req);
    return(-ENOTAVAILABLE);
  }
  atomic_store_explicit(&resp->hp_proc, proc, memory_order_release);
  resp->rc = 0;
  rc = lf_mpmc_queue_try_enqueue(&proc->queue, req);
  if(rc != LF_QUEUE_OK) {
    resp->rc = -ENOMEM;
    ERR_PRINT("lf_mpmc_queue_try_enqueue failed rc=%d\n", rc);
    return(-ENOMEM);
  }

  if(proc->threads_wait > 0 || proc->threads_run == 1) {
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

  if(!req) {
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
  eb = p_regfn->funcs[fidx].ext_block;

  retnum = req->retnum;
  if(flags == RPC_SEND_FIRST) {
    retnum = 1;
  }
  if(flags == RPC_SEND_RR) {
    rr_pos = atomic_fetch_add(&p_regfn->req_send_rr[fidx], 1);
    rr_pos = rr_pos % retnum;
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
    for(unsigned int i = 0; i < eb->szfn && it < retnum; i++) {
      proc = atomic_load(&eb->proc[i]);
      if(proc != NULL) {
        if(flags == RPC_SEND_LAST) { proc_last = proc; continue; }
        if(flags == RPC_SEND_RR && rr_pos != rr_cnt++) continue;
        rc = libsrpc_send_request_one(proc, req, it);
        if(rc == 0) it++;
      }
    }
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

  DBG_PRINT("wait for responses from customers\n");
  /* Ждём обработки всех запросов исполнителями. */
  unsigned int cnt_rcv;
  do{
    cnt_rcv = 0;
    rc = libsrpc_sem_wait_timeout_us(&req->sem_wakeup, timeout);
    if(rc < 0) {
      int err = errno;
      if(err == ETIMEDOUT) {
        /* Промаркируем пустые ответы как ошибки таймаута */
        for(unsigned int i = 0; i < retnum; i++) {
          srpc_rc_t expected = 0;
          libsrpc_response_t *resp = libsrpc_req_get_response(req, i);
          if(! resp) continue;
          atomic_compare_exchange_strong_explicit(&resp->rc, &expected, -ERPCWAITIMEDOUT, memory_order_acquire, memory_order_relaxed);
        }
        goto end;
      }
      if(err != EINTR && err != EAGAIN) {
        ERR_PRINT("unexpected error waiting for client '%s'\n", strerror(err));
        goto end;
      }
    }

    /* Подсчитаем полученные ответы. */
    for(unsigned int i = 0; i < retnum; i++) {
      libsrpc_response_t *resp = libsrpc_req_get_response(req, i);
      if(! resp) continue;
      if(atomic_load_explicit(&resp->hp_proc, memory_order_acquire) != NULL &&
         atomic_load_explicit(&resp->rc, memory_order_acquire) == 0) continue;
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

    /* Проверка запросов */
    for(int i = 0; i < proc->threads_num; i++) {
        void *hp = atomic_load_explicit(&proc->threads[i].hp_req, memory_order_acquire);
        if(hp != req) continue; /* Поток не блокирует запрос */
        return(1); /* Запрос заблокирован, сообщим вызывающему */
    }
  }
  return(0);
}

/* TODO: переделать на проверку по всем живым процессам, и убрать проверку corrupted и в req не заходить */
int libsrpc_req_is_busy(libsrpc_request_t *req)
{
  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(0);
  }

  for(unsigned int i = 0; i < req->retnum; i++) {
    libsrpc_response_t *resp = libsrpc_req_get_response(req, i);
    if(! resp) continue; /* Запрос некорректный */
    if(resp->rc != 0) continue; /* Уже ответил */
    if(! libsrpc_proc_is_valid(resp->hp_proc)) continue; /* Процесс невалидный */
    for(int j = 0; j < resp->hp_proc->threads_num; j++) {
      libsrpc_proc_thread_t *thread = &resp->hp_proc->threads[j];
      if(thread->hp_req != req) continue; /* Поток не блокирует запрос */
      return(1); /* Запрос заблокирован, сообщим вызывающему */
    }
  }

  return(0);
}


static int libsrpc_req_is_bad(libsrpc_request_t *req)
{
  if(!req) {
    return(1);
  }
  if(libsrpc_req_is_corrupted(req, NULL)) {
    return(1);
  }
  for(unsigned i = 0; i < req->retnum; i++) {
    libsrpc_response_t *resp = libsrpc_req_get_response(req, i);
    if(! resp) return(1);
    if((unsigned)resp->rc == LIBSRPC_RC_FLAG_LOCK ||
               resp->rc == -ERPCWAITIMEDOUT
      ) return(1);
  }

  return(0);
}

static void libsrpc_req_free(libsrpc_request_t *req, int is_bad)
{
  if(!req) {
    return;
  }
  libsrpc_list_remove(&req->node);
  libsrpc_req_destroy(req);
  if(is_bad) {
    libsrpc_shmem_free_dc(req);
    libsrpc_shm_gc_wakeup();
  }else{
    libsrpc_shmem_free(req);
  }
}

static _Atomic(uint32_t) libsrpc_req_seq_num = 0;

static libsrpc_request_t* libsrpc_req_alloc(int funid, int reqlen, int retsz)
{
  libsrpc_request_t *req = NULL;
  libsrpc_shmem_t *shm = libsrpc_shmem_get();
  libsrpc_proc_t *proc = simplerpc_data->proc;
  size_t sz = 0;
  int retnum = 0;
  int retoff = 0;

  if(!shm) {
    __libsrpc_errno_set(ESHMNOINIT);
    ERR_PRINT("shm is NULL\n");
    return(NULL);
  }

  srpc_regfn_shm_t *p_regfn = &shm->regfn;
  retnum = atomic_load(&p_regfn->funcs[sRPC_ID2IDX(funid)].num_all);
  if(retnum == 0) {
    __libsrpc_errno_set(ENOREGFUN);
    ERR_PRINT("The function '%s' is not linked\n", simplerpc_data->fn[sRPC_ID2IDX(funid)].name);
    return(NULL);
  }

  retoff = ALIGNLONG(reqlen);
  retsz  = ALIGNLONG(sizeof(libsrpc_response_t) + retsz);
  sz     = sizeof(libsrpc_request_t) + (size_t)reqlen + ((size_t)retsz * (size_t)retnum);

  int is_bad = libsrpc_req_is_bad(current_req);
  if(! is_bad &&
     current_req->bufsz >= sz
    ) { // Если помещается в текущий блок запроса и не завис, то используем его.
    req = current_req;
  }else{ // Если не помещается или не существует, то освобождаем текущий блок запроса и аллоцируем новый.
    req = current_req;
    current_req = NULL;
    if(req) {
      libsrpc_req_free(req, is_bad);
    }

    sz *= 2; // с запасом, следующие запросы могут быть больше текущего, снизим расходы на аллокацию.
    current_req = libsrpc_shmem_malloc_type(sz, LIBSRPC_SHMDT_REQUEST);
    req = current_req;
    if(req == NULL) {
      __libsrpc_errno_set(ENOMEM);
      ERR_PRINT("failed to allocate memory\n");
      return(NULL);
    }
    memset(req, 0, sz);
    req->sign = LIBSRPC_REQ_SIGN;
    libsrpc_sem_init(&req->sem_wakeup, 0);
    libsrpc_list_node_init(&req->node);
    libsrpc_list_push_front(&proc->req_head, &req->node);
  }

  req->seq_num = atomic_fetch_add(&libsrpc_req_seq_num, 1);
  req->funid  = funid;
  req->bufsz  = sz;
  req->retoff = retoff;
  req->retsz  = retsz;
  req->retnum = retnum;

  return(req);
}

/* ------------------------------------------------------------------------------ */
// Реализации функций, перехватывающих обёртки, для RPC вызовов.
#define XF(flags,rettype,name,...) \
static rettype sRPCFN(name)(M_ARGFUN(__VA_ARGS__)) { \
    int len=0; int rlen=0; int pos=0; \
    libsrpc_request_t *req = NULL; \
    __libsrpc_errno_clear(); \
    rlen = RLEN(rettype); \
    char rbuf[rlen]; \
    memset(rbuf, 0, rlen); \
    len = M_REQSIZE(__VA_ARGS__) 0; \
    req = libsrpc_req_alloc(GET_FNID(name), len, rlen); \
    if(req == NULL) { __libsrpc_errno_set(ENOMEM); }else{ \
      M_BODYFUN(__VA_ARGS__); \
      if(!!req && pos != len) { __libsrpc_errno_set(EBADMSG); }else{ \
        libsrpc_response_t *resp = libsrpc_req_get_response(req, 0); \
        libsrpc_send_request(req, flags); \
        libsrpc_req_response_get(resp, rbuf, rlen); \
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
                int pos = 0; \
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

static int libsrpc_dequeue_request(libsrpc_proc_t *proc, libsrpc_proc_thread_t *thread)
{
  int rc = 0;
  libsrpc_request_t *req = NULL;

  do{
    req = NULL;
    rc = 0;
    if(lf_mpmc_queue_try_dequeue(&proc->queue, (void**)&req) != LF_QUEUE_OK) {
      INF_PRINT("dequeue failed\n");
      break;
    }
    if(! req) continue;

    if(libsrpc_req_is_corrupted(req, NULL)) {
      ERR_PRINT("corrupted request: req=%p\n", req);
      continue;
    }
    libsrpc_req_ctrl_t ctrl = atomic_load_explicit(&req->control, memory_order_acquire);

    libsrpc_request_lock(thread, req);

    if(req->funid >= sRPCFNID_MAX || req->funid <= sRPCFNID_START) {
      libsrpc_request_unlock(thread);
      rc = -EBADMSG;
      ERR_PRINT("BAD REQUEST: req=%p funid=%d\n", req, req->funid);
      break;
    }

    /* Найдём свой слот для ответа на запрос. */
    libsrpc_response_t *resp = NULL;
    for(unsigned int i = 0; i < req->retnum; i++) {
      resp = libsrpc_req_get_response(req, i);
      if(! resp) continue;
      if(atomic_load_explicit(&resp->hp_proc, memory_order_acquire) != proc) continue;
      break;
    }
    if(! resp) {
      libsrpc_request_unlock(thread);
      rc = -EBADMSG;
      ERR_PRINT("BAD RESPONSE: req=%p resp=%p\n", req, resp);
      break;
    }

    INF_PRINT("dequeue success[%d]: req=%p funid=%d retsz=%d\n", sched_getcpu(), req, req->funid, req->retsz);

    rc = libsrpc_callback_func(req, resp, &ctrl);
    if((unsigned int)rc != LIBSRPC_RC_FLAG_READY) {
      libsrpc_request_unlock(thread);
      WRN_PRINT("callback function failed: rc=%d\n", rc);
      break;
    }
  }while(1);
    libsrpc_request_unlock(thread);

  return(rc);
}

// Функция потока приема и обработки запросов.
static void * libsrpc_thread_rcv_req(void *arg __attribute__((unused))) {
  int rc = 0;
  int tid = 0;
  libsrpc_proc_thread_t *thread = NULL;
  libsrpc_proc_t *proc = simplerpc_data->proc;
  srpc_disable_rpc_recursion = 1;

  DBG_PRINT("started CPU=%d\n", sched_getcpu());

  tid = atomic_fetch_add(&proc->threads_run, 1);
  if(tid >= proc->threads_num) {
    ERR_PRINT("threads_run overflow: tid=%d threads_num=%d\n", tid, proc->threads_num);
    return(NULL);
  }
  thread = &proc->threads[tid];

  while(! atomic_load_explicit(&srpc_thread_stop_flag, memory_order_acquire)) {
    DBG_PRINT("wait for request[%d]\n", sched_getcpu());
    atomic_fetch_add(&proc->threads_wait, 1);
    rc = libsrpc_sem_wait(&proc->sem_wakeup);
    atomic_fetch_sub(&proc->threads_wait, 1);
    DBG_PRINT("sem_wait wakeup success: rc=%d\n", rc);
    if(rc == -1 && errno == EINTR) continue;
    if(rc == -1) {
      ERR_PRINT("sem_wait failed: %s\n", strerror(errno));
      break;
    }
    rc = libsrpc_dequeue_request(proc, thread);
    if(rc < 0 && (unsigned)rc != -LIBSRPC_RC_FLAG_READY) {
      ERR_PRINT("dequeue failed: rc=%d\n", rc);
      //break;
    }
  }
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
      break;
    }
    if(! resp) continue;
    /* Запрос не был выполнен, установим RC код ошибки. */
    srpc_rc_t expected = 0;
    atomic_compare_exchange_strong_explicit(&resp->rc, &expected, -ECANCELLED, memory_order_acquire, memory_order_relaxed);
    if(atomic_load_explicit(&req->sign, memory_order_acquire) == LIBSRPC_REQ_SIGN) { // <= ИИ говорит без того будет UB, добавли чтоб он отстал :)
      libsrpc_sem_post(&req->sem_wakeup);
    }
  }
}

int libsrpc_thread_rcv_req_stop(void) {
  int rc = 0;
  libsrpc_proc_t *proc = simplerpc_data->proc;
  int n = atomic_load(&proc->threads_wait) + 1;

  atomic_store(&srpc_thread_stop_flag, 1);

  for(int i = 0; i < n; i++) {
    libsrpc_sem_post(&proc->sem_wakeup);
  }

  // Очищаем очередь запросов.
  libsrpc_thread_rcv_queue_clear(proc);

  // Ждём пока все потоки завершатся.
  while(atomic_load_explicit(&proc->threads_run, memory_order_acquire) > 0) {
    usleep(100);
  }

  DBG_PRINT("send stopped\n");
  return(rc);
}

/* ============================================================================== */
// Регистрация своих функций в RPC.

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
    memcpy(eb, old_eb, old_eb->size);
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
  int rc = 0;

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

  rc = -ENOMEM;
  for(int i = 0; i < 10; i++) {
    if(!eb) rc = libsrpc_regfn_ext_block(func);
    if(rc < 0) break;
    eb = func->ext_block;
    if(!eb) return(-ENOMEM);

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
    ptr = srpc.real_malloc(size);
    return ptr;
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      ptr = libsrpc_shmem_malloc(size);
      break;
    default:
      // Используем реальный malloc
      ptr = srpc.real_malloc(size);
      break;
  }

    return ptr;
}

void* calloc(size_t num, size_t size)
{
  void* ptr = NULL;

  if(!en_allocator) {
    ptr = srpc.real_calloc(num, size);
    return ptr;
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      ptr = libsrpc_shmem_calloc(num, size);
      break;
    default:
      ptr = srpc.real_calloc(num, size);
      break;
  }

  return(ptr);
}

void* realloc(void* oldptr, size_t newsize)
{
  void* ptr = NULL;

  if(!en_allocator) {
    ptr = srpc.real_realloc(oldptr, newsize);
    return(ptr);
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      ptr = libsrpc_shmem_realloc(oldptr, newsize);
      break;
    default:
      ptr = srpc.real_realloc(oldptr, newsize);
      break;
  }

  return(ptr);
}

// Переопределение free
void free(void* ptr) {

  if(!en_allocator) {
    srpc.real_free(ptr);
    return;
  }

  switch(switch_allocator) {
    case SRPC_ALLOC_SHM:
      libsrpc_shmem_free(ptr);
      break;
    default:
      // Используем реальный free
      srpc.real_free(ptr);
      break;
  }
}
#endif
/* ============================================================================== */
void spawn_daemon(const char *daemon_name);
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
void libsrpc_destructor(void) {
    DBG_PRINT("Библиотека выгружена: деструктор вызван PID=%d\n", getpid());
    if(simplerpc_data->is_daemon) {
      libsrpc_unix_server_exit(&simplerpc_data->srv);
    } else {
      libsrpc_unix_client_exit(&simplerpc_data->cli);
    }
}
/* ============================================================================== */


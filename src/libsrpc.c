
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>

#include <tlsf.h>

#include "libsrpc.h"
#include "libsrpc_local.h"
#include "libsrpc_errno.h"
#include "libsrpc_debug_print.h"
#include "libsrpc_pthread.h"
#include "libsrpc_mpmcq.h"
#include "libsrpc_futex.h"

/* ============================================================================== */
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
/* ============================================================================== */

#ifndef BUILD_TS
#define BUILD_TS "00000000000000" // Значение по умолчанию
#endif
static const char *daemon_name = "simplerpc_" BUILD_TS;

static simplerpc_t srpc;
simplerpc_t *simplerpc_data = &srpc;
/* ============================================================================== */
/* RPC */
static pthread_key_t srpc_key;
static pthread_once_t srpc_key_once = PTHREAD_ONCE_INIT;

void cleanup_my_data(void *ptr)
{
  free(ptr);
}

static void srpc_key_init(void) {
  pthread_key_create(&srpc_key, cleanup_my_data);
}

static void *srpc_get_data(void) {
  return pthread_getspecific(srpc_key);
}

static void srpc_set_data(void *ptr) {
  if(pthread_once(&srpc_key_once, srpc_key_init) != 0) {
    ERR_PRINT("pthread_once failed\n");
    __libsrpc_errno_set(EINTR);
    return;
  }
  pthread_setspecific(srpc_key, ptr);
}

/* ------------------------------------------------------------------------------ */
// Структура данных для отправки RPC запроса.
typedef struct srpc_req_s {
  job_futex_t jf; // futex для уведомления о завершении работы.
  atomic_int cnt_rcv; // Количество получивших запросы.
  atomic_int cnt_send; // Количество отправившых ответы.
  int funid; // ID функции.
  int bufsz; // Размер буфера данных.
  int retsz; // Размер одного ответа.
  int retnum; // Количество ответов в буфере.
  int retpos; // Позиция в буфере для размещения первого ответа.
  char buf[0]; // Буфер данных.
} srpc_req_t;

// Отправка RPC запроса.
static void sRPCsend(srpc_req_t *req)
{
  int rc = 0;
  libsrpc_shmem_t *shm = simplerpc_data->shmempool.shm;
  srpc_regfn_shm_t *p_regfn = &shm->regfn;

  if(!req) {
    __libsrpc_errno_set(EINVAL);
    return;
  }

  int retnum = req->retnum;
  srpc_regfn_block_t *block = &p_regfn->funcs[sRPC_ID2IDX(req->funid)].block;
  libsrpc_shmem_pid_t *pid = NULL;
  int it = 0;
DBG_PRINT("sRPCsend: send request to %d workers\n", retnum);
  // Отправляем запросы в очереди всех зарегистрированных процессов, которые имеют запущенные потоки.
  while(block && it < retnum) {
    for(int i = 0; i < SRPC_MAX_PID; i++) {
      if(block->pids[i] != NULL) {
        Request qreq = {.req = req, .retidx = it};
        pid = block->pids[i];
        if(!pid || pid->threads_run <= 0) continue;
        mpmc_queue_enqueue(&pid->queue, qreq);
        if(pid->threads_wait > 0) sem_post(&pid->sem);
        it++;
      }
    }
    block = block->next;
  }

  /* Ждём получения всех запросов исполнителями. */
  uint64_t timeout = 10;
  do{
    rc = job_futex_client_wait(&req->jf, timeout); // ждём 10 микросекунд.
    if(rc == 0 && atomic_load(&req->cnt_rcv) == req->retnum && atomic_load(&req->cnt_send) == req->retnum) {
      goto end; // Все запросы получены и исполнены.
    }
    if(rc == -ETIMEDOUT) {
      /* Если все запросы получены, то ждём исполнения всех запросов бесконечно. */
      if(atomic_load(&req->cnt_rcv) == req->retnum) timeout = JOB_FUTEX_INFINITE;
      DBG_PRINT("sRPCsend: timeout waiting for client\n");
    }else{
      /* Если ошибка не таймаут, то выходим. */
      ERR_PRINT("sRPCsend: unexpected error waiting for client\n");
      break;
    }
  }while(atomic_load(&req->cnt_rcv) != req->retnum || atomic_load(&req->cnt_send) != req->retnum);

end:
  DBG_PRINT("sRPCsend: wait for client done\n");
  return;
}

static srpc_req_t* libsrpc_alloc_req(int funid, int reqlen, int retsz)
{
  srpc_req_t *req = NULL;
  libsrpc_shmem_t *shm = simplerpc_data->shmempool.shm;
  if(!shm) {
    __libsrpc_errno_set(EINVAL);
    ERR_PRINT("libsrpc_alloc_req: shm is NULL\n");
    return(NULL);
  }
  
  srpc_regfn_shm_t *p_regfn = &shm->regfn;
  int retnum = p_regfn->funcs[sRPC_ID2IDX(funid)].num_all;
  size_t sz = sizeof(srpc_req_t) + reqlen + retsz*retnum;

  if(retnum == 0) {
    __libsrpc_errno_set(ENOREGFUN);
    ERR_PRINT("libsrpc_alloc_req: no registered functions '%s'\n", simplerpc_data->fn[sRPC_ID2IDX(funid)].name);
    return(NULL);
  }

  req = libsrpc_shmem_malloc(sz);
  if(req == NULL) {
    __libsrpc_errno_set(ENOMEM);
    return(NULL);
  }
  memset(req, 0, sz);
  job_futex_init(&req->jf, retnum * 2); // *2 потому что каждый запрос будет отправлен и получен.
  req->funid = funid;
  req->bufsz = sz;
  req->retsz = retsz;
  req->retnum = retnum;
  req->retpos = reqlen;

  return(req);
}

void libsrpc_free_req(srpc_req_t *req)
{
  if(!req) {
    return;
  }
  job_futex_destroy(&req->jf);
  libsrpc_shmem_free(req);
}

/* ------------------------------------------------------------------------------ */
// Реализации функций обёрток над RPC=вызовом.
#define XF(rettype,name,...) \
static rettype sRPCFN(name)(M_ARGFUN(__VA_ARGS__)) { \
    int len=0; int rlen=0; int pos=0; \
    srpc_req_t *req = NULL; \
    __libsrpc_errno_clear(); \
    if(strcmp("void",#rettype)!=0) rlen = sizeof(rettype); \
    char rbuf[rlen]; memset(rbuf, 0, rlen); \
    len = M_REQSIZE(__VA_ARGS__) 0; \
    req = libsrpc_alloc_req(GET_FNID(name), len, rlen); \
    if(req == NULL) { __libsrpc_errno_set(ENOMEM); }else{ \
      M_BODYFUN(__VA_ARGS__); \
      if(!!req && pos != len) { __libsrpc_errno_set(EBADMSG); }else{ \
        sRPCsend(req); \
        memcpy(rbuf, &req->buf[len], rlen); \
      } \
    } \
    DBG_PRINT("RPC call: %s %s(%s fnid=%d) len=%d retsz=%d\n", #rettype, #name, #__VA_ARGS__, GET_FNID(name), len, rlen); \
    RETDATA(rettype,rbuf); \
  }
RPC_LIST
#undef XF

// Прототипы ф-ий с атрибутом weak, для link-овки. Если функция определена в текущей приложении, будет вызвана она, иначе подмена.
#define XF(rettype,name,...) \
  __attribute__((weak, alias("librpcimp_" #name))) \
  rettype name(__VA_ARGS__);
RPC_LIST
#undef XF

// Указатели на функции, оригинал (если есть в текущем приложении) и подмену.
static const srpc_func_t srpc_fn[sRPC_FNNUM] = {
#define XF(rettype,fname,...)  {.rpc = sRPCFN(fname), .loc = fname, .name = #fname },
RPC_LIST
#undef XF
};

static simplerpc_t srpc = {
  .sign = {BUILD_TS},
  .fn = srpc_fn,
  .fn_sz = sRPC_FNNUM,
};

/* ============================================================================== */
/* SHARED MEMORY POLL and allocator */

enum {
  SRPC_ALLOC_STD = 0,
  SRPC_ALLOC_SHM,
};

static volatile char en_allocator = 0; // fucking TLS(thread local storage) |==:=>-.
_Thread_local static atomic_char switch_allocator = SRPC_ALLOC_STD;

#if 1
void srpc_alloc_sw_std(void) {
  switch_allocator = SRPC_ALLOC_STD;
}

void srpc_alloc_sw_shm(void) {
  switch_allocator = SRPC_ALLOC_SHM;
}

// Переопределение malloc
__attribute__((malloc,malloc(free)))
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
// Регистрация своих функций в RPC.
static int srpc_regfn_insert(srpc_regfn_block_t *block, libsrpc_shmem_pid_t *pid)
{
  int rc = -1;

  // Надйдём свободный элемент для вставки.
  while(block) {
    for(int i = 0; i < SRPC_MAX_PID; i++) {
      if(block->pids[i] == NULL) {
        block->pids[i] = pid;
        atomic_fetch_add(&block->num, 1);
        rc = 0;
        DBG_PRINT("!!! srpc_regfn_insert: %p insert '%s' OK\n", pid, srpc_fn[i].name);
        goto end;
      }
    }
    block = block->next;
  }

end:
  return(rc);
}

static int srpc_regfn_remove(srpc_regfn_block_t *block, libsrpc_shmem_pid_t *pid) 
{
  int rc = 0;

  while(block) {
    for(int i = 0; i < SRPC_MAX_PID; i++) {
      if(block->pids[i] == pid) {
        block->pids[i] = NULL;
        atomic_fetch_sub(&block->num, 1);
        rc++;
      }
    }
    block = block->next;
  }

  return(rc);
}

int srpc_regfn_rpc_shm(libsrpc_shmem_pid_t *fpid)
{
  int rc = 0;
  libsrpc_shmem_t *shm = simplerpc_data->shmempool.shm;
  srpc_regfn_shm_t *p_regfn = &shm->regfn;

  //sRPC_IDX2ID(i)
  pthread_mutex_lock(&p_regfn->mutex);
  for(int i = 0; i < sRPC_FNNUM; i++) {
    if(srpc_fn[i].loc != srpc_fn[i].rpc) {
      rc = srpc_regfn_insert(&p_regfn->funcs[i].block, fpid);
      if(rc == 0) {
        DBG_PRINT("!!! srpc_regfn_rpc_shm: %p insert '%s' OK\n", fpid, srpc_fn[i].name);
        atomic_fetch_add(&p_regfn->funcs[i].num_all, 1);
      } else {
        ERR_PRINT("srpc_regfn_rpc_shm: %p insert '%s' failed\n", fpid, srpc_fn[i].name);
      }
    }
  }
  pthread_mutex_unlock(&p_regfn->mutex);

  return(rc);
}

int srpc_unregfn_rpc_shm(libsrpc_shmem_pid_t *fpid)
{
  int rc = 0;
  libsrpc_shmem_t *shm = simplerpc_data->shmempool.shm;
  srpc_regfn_shm_t *p_regfn = &shm->regfn;

  pthread_mutex_lock(&p_regfn->mutex);
  for(int i = 0; i < sRPC_FNNUM; i++) {
    rc = srpc_regfn_remove(&p_regfn->funcs[i].block, fpid);
    if(rc > 0) atomic_fetch_sub(&p_regfn->funcs[i].num_all, rc);
    DBG_PRINT("!!! srpc_unregfn_rpc_shm: remove '%s' OK rc=%d\n", srpc_fn[i].name, rc);
  }
  pthread_mutex_unlock(&p_regfn->mutex);
  return(rc);
}

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
static int srpc_callback_func(srpc_req_t *req) 
{
  int rc = 0;

  atomic_fetch_add(&req->cnt_rcv, 1);
  job_futex_worker_done(&req->jf);
  switch(req->funid) {
    #define XF(rettype,name,...) \
        case GET_FNID(name): \
            /* Защита от рекурсии: если зарегистрирована сама обертка */ \
            if ((void*)&(name) == (void*)&(sRPCFN(name))) { rc = -1; break; } \
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
                    memcpy(&req->buf[pos], &retval, sizeof(retval)); \
                ) \
            } \
            break;
        RPC_LIST
    #undef XF
//    #define XF(rettype,name,...)   case sRPCFNID_##name: if( &(name) == &(sRPCFN(name)) ) break; break;
//        RPC_LIST
//    #undef XF
    default:
      ERR_PRINT("srpc_callback_func: BAD FUNID: funid=%d\n", req->funid);
      rc = -1;
      break;
  } // switch(req->funid)
  atomic_fetch_add(&req->cnt_send, 1);
  job_futex_worker_done(&req->jf);
  return(rc);
}
/* ------------------------------------------------------------------------------ */
// Поток приема и обработки запросов.
int sched_getcpu(void);
static atomic_char srpc_thread_stop_flag = 0;

// Функция потока приема и обработки запросов.
static void * srpc_thread_rcv_req(void *arg) {
  int rc = 0;
  int n = 0;
  Request qreq;
  libsrpc_shmem_pid_t *pi = simplerpc_data->pid_info;

  DBG_PRINT("!!! srpc_thread_rcv_req: started CPU=%d\n", sched_getcpu());

  atomic_fetch_add(&pi->threads_run, 1);
  while(!srpc_thread_stop_flag) {
    DBG_PRINT("srpc_thread_rcv_req: wait for request[%d]\n", sched_getcpu());
    atomic_fetch_add(&pi->threads_wait, 1);
    rc = sem_wait(&pi->sem);
    atomic_fetch_sub(&pi->threads_wait, 1);
    DBG_PRINT("sem_wait wakeup success[%d]: rc=%d\n", sched_getcpu(), rc);
    if(rc == -1 && errno == EINTR) continue;
    if(rc == -1) {
      ERR_PRINT("srpc_thread_rcv_req: sem_wait failed: %s\n", strerror(errno));
      break;
    }
DBG_PRINT("dequeue start[%d]: rc=%d\n", sched_getcpu(), n);
    do{
      n = mpmc_queue_try_dequeue(&pi->queue, &qreq);
      if(n == 0) {
        DBG_PRINT("dequeue empty[%d]: n=%d\n", sched_getcpu(), n);
        break;
      }
      srpc_req_t *sreq = qreq.req;
      if(!sreq || sreq->funid >= sRPCFNID_MAX || sreq->funid <= sRPCFNID_START) {
        ERR_PRINT("srpc_thread_rcv_req: BAD REQUEST: req=%p retsz=%d funid=%d\n", sreq, sreq->retsz, sreq->funid);
        break;
      }
      DBG_PRINT("dequeue success[%d]: req.req=%p funid=%d retidx=%d retsz=%d\n", sched_getcpu(), sreq, sreq->funid, qreq.retidx, sreq->retsz);
      rc = srpc_callback_func(sreq);
    }while(n > 0);
DBG_PRINT("dequeue end[%d]: rc=%d\n", sched_getcpu(), n);
  }
  atomic_fetch_sub(&pi->threads_run, 1);

  DBG_PRINT("!!! srpc_thread_rcv_req: stopped CPU=%d\n", sched_getcpu());
  return(NULL);
}

int srpc_thread_rcv_req_start(void) {
  int rc = 0;
  rc = pthread_start_all_cpu(srpc_thread_rcv_req, NULL);
  if (rc != 0) {
    ERR_PRINT("srpc_thread_rcv_req_start: pthread_start_all_cpu failed\n");
    goto err;
  }
  DBG_PRINT("srpc_thread_rcv_req_start: started\n");
out:
  return(rc);
err:
  return(rc);
  goto out;
}

// Очистка очереди запросов.
void srpc_thread_rcv_queue_clear(libsrpc_shmem_pid_t *pi)
{
  Request req;
  
  while(mpmc_queue_try_dequeue(&pi->queue, &req)) {
    //!!! libsrpc_shmem_free(req.value);
  }
}

int srpc_thread_rcv_req_stop(void) {
  int rc = 0;
  libsrpc_shmem_pid_t *pi = simplerpc_data->pid_info;
  int n = atomic_load(&pi->threads_wait) + 1;

  atomic_store(&srpc_thread_stop_flag, 1);

  for(int i = 0; i < n; i++) {
    sem_post(&simplerpc_data->pid_info->sem);
  }

  // Очищаем очередь запросов.
  srpc_thread_rcv_queue_clear(pi);

  // Ждём пока все потоки завершатся.
  while(pi->threads_wait > 0) {
    usleep(100);
  }
  
  DBG_PRINT("srpc_thread_rcv_req_stop: send stopped\n");
  return(rc);
}

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
    if (!srpc.real_malloc || !srpc.real_free || !srpc.real_calloc || !srpc.real_realloc) {
        const char *error_msg = "Error loading malloc/free: dlsym failed.\n";
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        exit(1);  // Завершаем программу, если не удалось загрузить настоящие функции
    }
    //DBG_PRINT("malloc %p:%p calloc %p:%p realloc %p:%p free %p:%p\n", srpc.real_malloc, malloc, srpc.real_calloc, calloc, srpc.real_realloc, realloc, srpc.real_free, free);

    DBG_PRINT("Программа %s PID=%d скомпилирована: %s\n", program_invocation_name, getpid(), BUILD_TS );
    DBG_PRINT("daemon_name = %s  program_invocation_name = %s\n", daemon_name, program_invocation_name);

    if(!!strcmp(program_invocation_name, daemon_name)) {
      DBG_PRINT("spawn_daemon\n");
      spawn_daemon(daemon_name);
      en_allocator = 1;
      libsrpc_unix_client_init(&simplerpc_data->cli, daemon_name);
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



#include <string.h>
#include <stdatomic.h>
#include "tlsf_txn.h"

#include "libsrpc_errno.h"
#include "libsrpc_shmem.h"
#include "libsrpc_debug_print.h"
#include "libsrpc_wrapper.h"

#include "libsrpc_local.h"
#include "libsrpc_proc.h"
#include "libsrpc_shm_gc.h"


static uint16_t libsrpc_proc_get_uid(void)
{
    static atomic_uint_least16_t libsrpc_proc_uid_next = DAEMON_PROC_UID + 1;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    libsrpc_proc_t *proc = NULL;

    if (shm == NULL) {
        __libsrpc_errno_set(ESHMNOINIT);
        return 0;
    }

    int cnt = 0;
find:
    LIBSRPC_LIST_FOREACH(&shm->proc_head, proc, libsrpc_proc_t, proc_node) {
        if (proc->proc_uid == libsrpc_proc_uid_next) {
            libsrpc_proc_uid_next++;
            if(libsrpc_proc_uid_next == 0) {
                libsrpc_proc_uid_next = DAEMON_PROC_UID + 1;
                return 0;
            }
            if(++cnt > 100) {
                return 0;
            }
            goto find;
        }
    }

    return(libsrpc_proc_uid_next);
}


libsrpc_proc_t * libsrpc_proc_get(pid_t pid)
{
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    libsrpc_proc_t *proc = NULL;

    if (shm == NULL) {
        __libsrpc_errno_set(ESHMNOINIT);
        return NULL;
    }

    LIBSRPC_LIST_FOREACH(&shm->proc_head, proc, libsrpc_proc_t, proc_node) {
        if (proc->pid == pid) {
            return proc;
        }
    }

    return NULL;
}

int libsrpc_proc_create(pid_t pid)
{
    int rc = 0;
    size_t size = 0;
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    libsrpc_proc_t *proc = NULL;
    long threads_num = sysconf(_SC_NPROCESSORS_ONLN);

    if (threads_num <= 0 || threads_num > 1024 || pid <= 0) {
        return -EINVAL;
    }

    if (shm == NULL) {
        return -ESHMNOINIT;
    }

    proc = libsrpc_proc_get(pid);
    if (proc != NULL) {
        return -EEXIST;
    }

    size = sizeof(libsrpc_proc_t) + (size_t)threads_num * sizeof(libsrpc_proc_thread_t);
    if (size < sizeof(libsrpc_proc_t)) {
        return -EINVAL;
    }
    proc = libsrpc_shmem_malloc_type(size, LIBSRPC_SHMDT_PROC);
    if (proc == NULL) {
        return -ENOMEM;
    }

    memset(proc, 0, size);
    proc->pid = pid;
    proc->proc_uid = libsrpc_proc_get_uid();
    proc->threads_num = (unsigned int)threads_num;
    if(proc->proc_uid == 0) {
        rc = -EBADF;
        ERR_PRINT("proc_uid is 0\n");
        goto err;
    }

    rc = libsrpc_sem_init(&proc->sem_wakeup, 0);
    if(rc < 0) {
        ERR_PRINT("sem_init failed: %s\n", libsrpc_strerror(errno));
        goto err;
    }

    lf_mpmc_queue_init(&proc->queue);

    rc = libsrpc_list_head_init(&proc->req_head);
    if(rc < 0) {
        ERR_PRINT("list_head_init failed: %s\n", libsrpc_strerror(-rc));
        goto err;
    }

    atomic_store_explicit(&proc->sign, LIBSRPC_PROC_SIGN, memory_order_release);
    rc = libsrpc_list_push_front(&shm->proc_head, &proc->proc_node);
    if(rc < 0) {
        ERR_PRINT("list_push_front failed: %s\n", libsrpc_strerror(-rc));
        goto err;
    }

    DBG_PRINT("proc=%p created\n", (void*)proc);

    return(0);
err:
    if(proc) libsrpc_shmem_free(proc);
    return(rc);
}


typedef struct destructor_fn_s {
    size_t dc_count;
    libsrpc_shm_trash_t *trash;
} destructor_fn_t;

static int destructor_fn(tlsf_t tlsf, void *ptr, block_header_t *block, uint16_t match_uid, void *user)
{
    int rc = 0;
    int data_type = 0;
    int flag_dc = 0;
    destructor_fn_t *destructor_data = (destructor_fn_t *)user;

    if(ptr == NULL || tlsf == NULL || block == NULL || destructor_data == NULL) {
        return -EINVAL;
    }

    data_type = tlsf_get_data_type(tlsf, ptr);
    flag_dc = tlsf_getdc(tlsf, ptr);

    DBG_PRINT("DESTRUCTOR ptr=%p, match_uid=%d, data_type=%d\n", ptr, match_uid, data_type);

    switch(data_type) {
        case LIBSRPC_SHMDT_REGFN: /* Выделяет только демон, если пришло сюда, значит ошибка в коде */
        case LIBSRPC_SHMDT_PROC: /* Выделяет только демон, если пришло сюда, значит ошибка в коде */
            ERR_PRINT("!!! DESTRUCTOR REGFN/PROC ptr=%p, match_uid=%d\n", ptr, match_uid);
            break;
        case LIBSRPC_SHMDT_REQUEST:
            if(flag_dc == 0) { // Специальные блоки в отложенную очистку, чтоб избежать гонки.
                libsrpc_req_destroy(ptr); // Уничтожаем запрос
                tlsf_setdc(tlsf, ptr); // Устанавливаем флаг для отложенной очистки
                destructor_data->dc_count++;
            }
            break;
        case LIBSRPC_SHMDT_USER:
        default:
            if(destructor_data->trash != NULL) {
                libsrpc_shm_trash_add_ptr(&destructor_data->trash, ptr);
                DBG_PRINT("!!! DESTRUCTOR trash=%p, ptr=%p, match_uid=%d\n", (void*)destructor_data->trash, ptr, match_uid);
            }else{
                tlsf_free_nb(tlsf, ptr, match_uid);
                rc = 1; /* Сообщим что удалили */
            }
            break;
    }

    return(rc);
}


int libsrpc_proc_destroy(pid_t pid)
{
    int rc = 0;
    libsrpc_proc_t *proc = libsrpc_proc_get(pid);
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    destructor_fn_t destructor_data = {0};

    if (proc == NULL) {
        return -EINVAL;
    }

    libsrpc_unreg_func_proc(proc);/* Удаление функций из реестра */

    atomic_store_explicit(&proc->sign, 0, memory_order_release);
    atomic_store_explicit(&proc->status, LIBSRPC_PROC_STATUS_DELETE, memory_order_seq_cst);

    /* Удаление процесса из списка процессов */
    rc = libsrpc_list_remove(&proc->proc_node);
    if(rc < 0) {
        ERR_PRINT("list_remove failed: %s\n", libsrpc_strerror(errno));
    }

    /* sem_destroy делает GC, когда ни один слот ответа не держит hp_proc на этот proc. */

    libsrpc_thread_rcv_queue_clear(proc); /* Очистка очереди приёма сообщений */

    /* Освобождение забытых блоков памяти из пула */
    if(libsrpc_shmem_is_locked_uid(proc->proc_uid)) {
        DBG_PRINT("!!! proc=%p is locked\n", (void*)proc);
        destructor_data.trash = libsrpc_shm_trash_create_node(1024);
        if(destructor_data.trash == NULL) {
            ERR_PRINT("gc_data_create_node failed: %s\n", libsrpc_strerror(errno));
            return -ENOMEM;
        }
        destructor_data.trash->uid = proc->proc_uid;

        tlsf_free_uid_blocks((tlsf_t)shm->poolptr, proc->proc_uid, destructor_fn, &destructor_data);
        if(destructor_data.dc_count > 0) {
            libsrpc_shm_gc_wakeup(); /* разбудим сборщик мусора */
            DBG_PRINT("DC count: %zu\n", destructor_data.dc_count);
        }

        rc = libsrpc_shm_trash_add_node(destructor_data.trash);
        if(rc < 0) {
            ERR_PRINT("gc_data_add_node failed: %s\n", libsrpc_strerror(-rc));
            return -EINVAL;
        }
    }else{
        tlsf_free_uid_blocks((tlsf_t)shm->poolptr, proc->proc_uid, destructor_fn, &destructor_data);
        if(destructor_data.dc_count > 0) {
            libsrpc_shm_gc_wakeup(); /* разбудим сборщик мусора */
            DBG_PRINT("DC count: %zu\n", destructor_data.dc_count);
        }
    }

    /* Отложенное освобождение памяти proc */
    libsrpc_shmem_free_dc(proc);
    libsrpc_shm_gc_wakeup();

    return rc;
}

static int libsrpc_req_list_aims_at(libsrpc_list_head_t *head, libsrpc_proc_t *target)
{
    libsrpc_request_t *req = NULL;

    LIBSRPC_LIST_FOREACH(head, req, libsrpc_request_t, node) {
        unsigned int n = req->retnum;
        for (unsigned int i = 0; i < n; i++) {
            libsrpc_response_t *resp = libsrpc_req_get_response_fast(req, i);
            if (!resp) {
                continue;
            }
            srpc_rc_t rrc = atomic_load_explicit(&resp->rc, memory_order_seq_cst);
            if (rrc.rc == 0 && 
                atomic_load_explicit(&resp->hp_proc, memory_order_seq_cst) == target ) {
                return 1;
            }
        }
    }
    return 0;
}

int libsrpc_proc_cs_aimed_at(libsrpc_proc_t *target)
{
    libsrpc_shmem_t *shm = libsrpc_shmem_get();
    libsrpc_proc_t *proc = NULL;

    if (!target || !shm) {
        return 0;
    }

    LIBSRPC_LIST_FOREACH(&shm->proc_head, proc, libsrpc_proc_t, proc_node) {
        if (libsrpc_req_list_aims_at(&proc->req_head, target)) {
            return 1;
        }
    }
    return 0;
}


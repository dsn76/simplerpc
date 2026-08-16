/* Qwen DList implementation */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "libsrpc_dlist.h"
#include <string.h>

#if !defined(PTHREAD_MUTEX_ROBUST) && defined(PTHREAD_MUTEX_ROBUST_NP)
#define PTHREAD_MUTEX_ROBUST PTHREAD_MUTEX_ROBUST_NP
#endif

enum {
    DL_TX_NONE = 0,
    DL_TX_INSERT = 1,
    DL_TX_REMOVE = 2
};

/*
 * Барьеры и атомарные операции нужны, чтобы журнал транзакции и изменения
 * списка наблюдались другими процессами в правильном порядке.
 */
static inline void dl_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline uint32_t dl_load_state(dlist_head_t *h)
{
    return __atomic_load_n(&h->tx_state, __ATOMIC_SEQ_CST);
}

static inline void dl_store_state(dlist_head_t *h, uint32_t v)
{
    __atomic_store_n(&h->tx_state, v, __ATOMIC_SEQ_CST);
}

static inline dlist_elem_t *dl_load_ptr(dlist_elem_t **p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static inline void dl_store_ptr(dlist_elem_t **p, dlist_elem_t *v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static void dl_commit_tx_locked(dlist_head_t *h)
{
    dl_fence();
    dl_store_state(h, DL_TX_NONE);
}

static void dl_begin_tx_locked(dlist_head_t *h,
                               uint32_t op,
                               dlist_elem_t *node,
                               dlist_elem_t *prev,
                               dlist_elem_t *next)
{
    __atomic_store_n(&h->tx_node, node, __ATOMIC_SEQ_CST);
    __atomic_store_n(&h->old_prev, prev, __ATOMIC_SEQ_CST);
    __atomic_store_n(&h->old_next, next, __ATOMIC_SEQ_CST);

    dl_fence();
    dl_store_state(h, op);
    dl_fence();
}

/*
 * Откат незавершенной транзакции.
 * Вызывать только удерживая mutex.
 */
static void dl_rollback_locked(dlist_head_t *h)
{
    uint32_t st = dl_load_state(h);

    if (st == DL_TX_NONE)
        return;

    dlist_elem_t *node = dl_load_ptr(&h->tx_node);
    dlist_elem_t *prev = dl_load_ptr(&h->old_prev);
    dlist_elem_t *next = dl_load_ptr(&h->old_next);

    /*
     * Если журнал поврежден или указатели невалидны, очищаем транзакцию,
     * чтобы не блокировать список навсегда.
     *
     * В production здесь можно вместо этого возвращать ошибку и помечать
     * список как поврежденный.
     */
    if (!node || !prev || !next) {
        dl_commit_tx_locked(h);
        return;
    }

    if (st == DL_TX_INSERT) {
        /*
         * Откат вставки: элемент не должен остаться в списке.
         * Возвращаем связь prev <-> next.
         */
        dl_store_ptr(&prev->next, next);
        dl_store_ptr(&next->prev, prev);

        dl_store_ptr(&node->prev, NULL);
        dl_store_ptr(&node->next, NULL);
    } else if (st == DL_TX_REMOVE) {
        /*
         * Откат удаления: возвращаем элемент между prev и next.
         */
        dl_store_ptr(&node->prev, prev);
        dl_store_ptr(&node->next, next);

        dl_store_ptr(&prev->next, node);
        dl_store_ptr(&next->prev, node);
    }

    dl_commit_tx_locked(h);
}

int dlist_init(dlist_head_t *h)
{
    pthread_mutexattr_t attr;
    int rc;

    if (!h)
        return EINVAL;

    memset(h, 0, sizeof(*h));

    h->magic = DL_MAGIC;
    h->version = 1;
    h->self = h;

    dl_store_ptr(&h->anchor.prev, &h->anchor);
    dl_store_ptr(&h->anchor.next, &h->anchor);

    rc = pthread_mutexattr_init(&attr);
    if (rc)
        return rc;

    rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (!rc)
        rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    if (!rc)
        rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (!rc)
        rc = pthread_mutex_init(&h->mutex, &attr);

    pthread_mutexattr_destroy(&attr);

    return rc;
}

int dlist_open(dlist_head_t *h)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    return 0;
}

int dlist_lock(dlist_head_t *h)
{
    int rc;

    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    rc = pthread_mutex_lock(&h->mutex);

    if (rc == EOWNERDEAD) {
        rc = pthread_mutex_consistent(&h->mutex);
        if (rc)
            return rc;

        dl_rollback_locked(h);
        return 0;
    }

    return rc;
}

int dlist_unlock(dlist_head_t *h)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    return pthread_mutex_unlock(&h->mutex);
}

void dlist_recover_locked(dlist_head_t *h)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return;

    dl_rollback_locked(h);
}

void dlist_elem_init(dlist_elem_t *e)
{
    if (!e)
        return;

    e->prev = NULL;
    e->next = NULL;
}

/*
 * Вставка e между prev и next.
 * Вызывать только под mutex.
 */
static int dl_do_insert_locked(dlist_head_t *h,
                               dlist_elem_t *e,
                               dlist_elem_t *prev,
                               dlist_elem_t *next)
{
    if (!e || !prev || !next)
        return EINVAL;

    if (e == &h->anchor)
        return EINVAL;

    /*
     * Элемент уже не должен быть в списке.
     * У отключенного элемента prev == next == NULL.
     */
    if (dl_load_ptr(&e->prev) != NULL || dl_load_ptr(&e->next) != NULL)
        return EBUSY;

    /*
     * Проверяем целостность места вставки.
     */
    if (dl_load_ptr(&prev->next) != next ||
        dl_load_ptr(&next->prev) != prev) {
        return EINVAL;
    }

    dl_begin_tx_locked(h, DL_TX_INSERT, e, prev, next);

    dl_store_ptr(&e->prev, prev);
    dl_store_ptr(&e->next, next);

    dl_store_ptr(&prev->next, e);
    dl_store_ptr(&next->prev, e);

    dl_commit_tx_locked(h);

    return 0;
}

/*
 * Удаление e из списка.
 * Вызывать только под mutex.
 */
static int dl_do_remove_locked(dlist_head_t *h, dlist_elem_t *e)
{
    if (!e)
        return EINVAL;

    if (e == &h->anchor)
        return EINVAL;

    dlist_elem_t *prev = dl_load_ptr(&e->prev);
    dlist_elem_t *next = dl_load_ptr(&e->next);

    /*
     * Элемент не в списке.
     */
    if (!prev || !next)
        return ENOENT;

    /*
     * Проверяем, что элемент действительно находится между prev и next.
     */
    if (dl_load_ptr(&prev->next) != e ||
        dl_load_ptr(&next->prev) != e) {
        return EINVAL;
    }

    dl_begin_tx_locked(h, DL_TX_REMOVE, e, prev, next);

    dl_store_ptr(&prev->next, next);
    dl_store_ptr(&next->prev, prev);

    dl_store_ptr(&e->prev, NULL);
    dl_store_ptr(&e->next, NULL);

    dl_commit_tx_locked(h);

    return 0;
}

int dlist_push_front_locked(dlist_head_t *h, dlist_elem_t *e)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    return dl_do_insert_locked(h, e, &h->anchor, dl_load_ptr(&h->anchor.next));
}

int dlist_push_back_locked(dlist_head_t *h, dlist_elem_t *e)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    return dl_do_insert_locked(h, e, dl_load_ptr(&h->anchor.prev), &h->anchor);
}

int dlist_pop_front_locked(dlist_head_t *h, dlist_elem_t **out)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    dlist_elem_t *e = dl_load_ptr(&h->anchor.next);

    if (!e)
        return EINVAL;

    if (e == &h->anchor)
        return ENOENT;

    int rc = dl_do_remove_locked(h, e);
    if (!rc && out)
        *out = e;

    return rc;
}

int dlist_remove_locked(dlist_head_t *h, dlist_elem_t *e)
{
    if (!h || h->magic != DL_MAGIC || h->self != h)
        return EINVAL;

    return dl_do_remove_locked(h, e);
}

int dlist_push_front(dlist_head_t *h, dlist_elem_t *e)
{
    int rc = dlist_lock(h);
    if (rc)
        return rc;

    rc = dlist_push_front_locked(h, e);

    int urc = dlist_unlock(h);
    return rc ? rc : urc;
}

int dlist_push_back(dlist_head_t *h, dlist_elem_t *e)
{
    int rc = dlist_lock(h);
    if (rc)
        return rc;

    rc = dlist_push_back_locked(h, e);

    int urc = dlist_unlock(h);
    return rc ? rc : urc;
}

int dlist_pop_front(dlist_head_t *h, dlist_elem_t **out)
{
    int rc = dlist_lock(h);
    if (rc)
        return rc;

    rc = dlist_pop_front_locked(h, out);

    int urc = dlist_unlock(h);
    return rc ? rc : urc;
}

int dlist_remove(dlist_head_t *h, dlist_elem_t *e)
{
    int rc = dlist_lock(h);
    if (rc)
        return rc;

    rc = dlist_remove_locked(h, e);

    int urc = dlist_unlock(h);
    return rc ? rc : urc;
}

int dlist_is_empty(dlist_head_t *h, int *out_empty)
{
    if (!h || !out_empty)
        return EINVAL;

    int rc = dlist_lock(h);
    if (rc)
        return rc;

    *out_empty = (dl_load_ptr(&h->anchor.next) == &h->anchor);

    return dlist_unlock(h);
}



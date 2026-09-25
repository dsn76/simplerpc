/* TZ: DeepSeek (Chat)
 * TZ fix: Claude Sonnet 5 Medium (Chat)
 * Implementation & fix TZ: MiMo V2.5 Free (Agent) */

#include "lf_mpmc_queue.h"

void lf_mpmc_queue_init(lf_mpmc_queue_t* q) {
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);

    for (size_t i = 0; i < LF_MPMC_QUEUE_CAPACITY; ++i) {
        atomic_store_explicit(&q->buffer[i].seq, i, memory_order_relaxed);
        q->buffer[i].data = NULL;
    }

    q->mask = LF_MPMC_QUEUE_CAPACITY - 1;
}

lf_queue_result_t lf_mpmc_queue_try_enqueue(lf_mpmc_queue_t* q, void* data) {
    size_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);

    for (;;) {
        size_t idx = pos & q->mask;
        size_t seq_old = atomic_load_explicit(&q->buffer[idx].seq, memory_order_acquire);
        ptrdiff_t diff = (ptrdiff_t)(seq_old - pos);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->head, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                q->buffer[idx].data = data;
                atomic_store_explicit(&q->buffer[idx].seq, pos + 1, memory_order_release);
                return LF_QUEUE_OK;
            }
        } else if (diff < 0) {
            return LF_QUEUE_FULL;
        } else {
            pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        }
    }
}

lf_queue_result_t lf_mpmc_queue_try_dequeue(lf_mpmc_queue_t* q, void** data) {
    size_t pos = atomic_load_explicit(&q->tail, memory_order_relaxed);

    for (;;) {
        size_t idx = pos & q->mask;
        size_t seq_old = atomic_load_explicit(&q->buffer[idx].seq, memory_order_acquire);
        ptrdiff_t diff = (ptrdiff_t)(seq_old - (pos + 1));

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->tail, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                *data = q->buffer[idx].data;
                atomic_store_explicit(&q->buffer[idx].seq, pos + LF_MPMC_QUEUE_CAPACITY, memory_order_release);
                return LF_QUEUE_OK;
            }
        } else if (diff < 0) {
            return LF_QUEUE_EMPTY;
        } else {
            pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        }
    }
}

lf_queue_result_t lf_mpmc_queue_try_find_ptr(lf_mpmc_queue_t* q, void* ptr) {
    if (!ptr) {
        return LF_QUEUE_NOT_FOUND;
    }

    /* tail раньше head: диапазон [tail, head) не теряет слоты, которые dequeue
     * забрал между двумя загрузками — такие pos отсечёт проверка tail ниже. */
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (head <= tail) {
        return LF_QUEUE_NOT_FOUND;
    }

    size_t n = head - tail;
    if (n > LF_MPMC_QUEUE_CAPACITY) {
        tail = head - LF_MPMC_QUEUE_CAPACITY;
        n = LF_MPMC_QUEUE_CAPACITY;
    }

    for (size_t i = 0; i < n; ++i) {
        size_t pos = tail + i;
        size_t idx = pos & q->mask;
        size_t seq = atomic_load_explicit(&q->buffer[idx].seq, memory_order_acquire);

        /* Опубликован и ещё не закрыт dequeue: enqueue пишет seq = pos + 1. */
        if (seq != pos + 1) {
            continue;
        }

        void* data = q->buffer[idx].data;
        seq = atomic_load_explicit(&q->buffer[idx].seq, memory_order_acquire);
        if (seq != pos + 1 || data != ptr) {
            continue;
        }

        /* Dequeue уже забрал слот (CAS tail), даже если seq ещё не сдвинут. */
        size_t tail_now = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (tail_now > pos) {
            continue;
        }

        return LF_QUEUE_OK;
    }

    return LF_QUEUE_NOT_FOUND;
}


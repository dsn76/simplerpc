/* Deepseek MPMCQ implementation */
#include "libsrpc_errno.h"

#include "libsrpc_mpmcq.h"


void mpmc_queue_init(MpmcQueue *q) {
    for (size_t i = 0; i < MPMC_QUEUE_CAPACITY; ++i) {
        atomic_init(&q->cells[i].sequence, i);
    }
    atomic_init(&q->enqueue_pos, 0);
    atomic_init(&q->dequeue_pos, 0);
}

void mpmc_queue_enqueue(MpmcQueue *q, Request req) {
    uint64_t pos = atomic_fetch_add_explicit(&q->enqueue_pos, 1, memory_order_relaxed);
    MpmcCell *cell = &q->cells[pos & (MPMC_QUEUE_CAPACITY - 1)];

    while (atomic_load_explicit(&cell->sequence, memory_order_acquire) != pos) {
        // spin / _mm_pause()
    }

    cell->data = req;
    atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release);
}

int mpmc_queue_try_enqueue(MpmcQueue *q, Request req) {
    uint64_t pos = 0;
    MpmcCell *cell = NULL;
    do {
        pos = atomic_load_explicit(&q->enqueue_pos, memory_order_acquire);
        cell = &q->cells[pos & (MPMC_QUEUE_CAPACITY - 1)];
        uint64_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        if (seq != pos) {
            // Ячейка занята – очередь заполнена
            return 0;
        }
        // Ячейка свободна, пытаемся продвинуть enqueue_pos
    } while (!atomic_compare_exchange_weak_explicit(
                 &q->enqueue_pos, &pos, pos + 1,
                 memory_order_acq_rel, memory_order_acquire));

    if (cell == NULL) {
        return -EBADFD;
    }
    // Успешно заняли ячейку – записываем данные и публикуем
    cell = &q->cells[pos & (MPMC_QUEUE_CAPACITY - 1)];
    cell->data = req;
    atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release);
    return 1;
}

Request mpmc_queue_dequeue(MpmcQueue *q) {
    uint64_t pos = atomic_fetch_add_explicit(&q->dequeue_pos, 1, memory_order_relaxed);
    MpmcCell *cell = &q->cells[pos & (MPMC_QUEUE_CAPACITY - 1)];

    while (atomic_load_explicit(&cell->sequence, memory_order_acquire) != pos + 1) {
        // spin / _mm_pause()
    }

    Request req = cell->data;
    atomic_store_explicit(&cell->sequence, pos + MPMC_QUEUE_CAPACITY, memory_order_release);
    return req;
}

int mpmc_queue_try_dequeue(MpmcQueue *q, Request *req) {
    uint64_t pos = 0;
    MpmcCell *cell = NULL;
    do {
        pos = atomic_load_explicit(&q->dequeue_pos, memory_order_acquire);
        cell = &q->cells[pos & (MPMC_QUEUE_CAPACITY - 1)];
        uint64_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        if (seq != pos + 1) {
            return 0;   // ячейка не готова — очередь пуста
        }
        // ячейка готова, пробуем зафиксировать её за собой
    } while (!atomic_compare_exchange_weak_explicit(
                 &q->dequeue_pos, &pos, pos + 1,
                 memory_order_acq_rel, memory_order_acquire));

    if (cell == NULL) {
        return -EBADFD;
    }
    // Успешно захватили элемент
    *req = cell->data;
    atomic_store_explicit(&cell->sequence, pos + MPMC_QUEUE_CAPACITY, memory_order_release);
    return 1;
}


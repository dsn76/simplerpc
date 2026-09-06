#ifndef LF_MPMC_QUEUE_H
#define LF_MPMC_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========================= КОНФИГУРАЦИЯ =========================
#define LF_MPMC_QUEUE_CAPACITY MPMC_QUEUE_CAPACITY
#ifndef LF_MPMC_QUEUE_CAPACITY
    #define LF_MPMC_QUEUE_CAPACITY 1024
#endif

#if (LF_MPMC_QUEUE_CAPACITY & (LF_MPMC_QUEUE_CAPACITY - 1)) != 0
    #error "LF_MPMC_QUEUE_CAPACITY must be a power of two"
#endif

// ========================= КОДЫ ОШИБОК =========================

typedef enum {
    LF_QUEUE_OK    = 0,
    LF_QUEUE_FULL  = -1,
    LF_QUEUE_EMPTY = -2
} lf_queue_result_t;

// ========================= СТРУКТУРА ОЧЕРЕДИ =========================

typedef struct {
    alignas(64) atomic_size_t head;
    alignas(64) atomic_size_t tail;

    struct {
        alignas(64) atomic_size_t seq;
        void* data;
    } buffer[LF_MPMC_QUEUE_CAPACITY];

    size_t mask;
} lf_mpmc_queue_t;

// ========================= ИНИЦИАЛИЗАЦИЯ =========================

void lf_mpmc_queue_init(lf_mpmc_queue_t* q);

// ========================= ОПЕРАЦИИ =========================

lf_queue_result_t lf_mpmc_queue_try_enqueue(lf_mpmc_queue_t* q, void* data);

lf_queue_result_t lf_mpmc_queue_try_dequeue(lf_mpmc_queue_t* q, void** data);

#ifdef __cplusplus
}
#endif

#endif // LF_MPMC_QUEUE_H

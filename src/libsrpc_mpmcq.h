#ifndef FILE_LIBSRPC_MPMCQ_H
#define FILE_LIBSRPC_MPMCQ_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MPMC_QUEUE_CAPACITY
#define MPMC_QUEUE_CAPACITY 1024   // обязательно степень двойки
#endif

enum {
    REQUEST_STATUS_WAITING = 0,
    REQUEST_STATUS_EXECUTING = 1,
    REQUEST_STATUS_CANCELLED = 2,
    REQUEST_STATUS_COMPLETED = 3,
};

typedef struct {
    void *req;  // Указатель на запрос.
    int retidx; // Индекс ответа в буфере.
    atomic_char status; // Статус запроса: 0 - ожидает, 1 - исполняется, 2 - отменён, 3 - завершён.
} Request;

typedef struct {
    _Atomic uint64_t sequence;
    Request data;
} __attribute__((aligned(64))) MpmcCell;

typedef struct {
    MpmcCell cells[MPMC_QUEUE_CAPACITY];
    _Atomic uint64_t enqueue_pos __attribute__((aligned(64)));
    _Atomic uint64_t dequeue_pos __attribute__((aligned(64)));
} __attribute__((aligned(64))) MpmcQueue;

// Однократная инициализация владельцем разделяемой памяти
void mpmc_queue_init(MpmcQueue *q);

// Блокирующая вставка (ждёт, пока не освободится место)
void mpmc_queue_enqueue(MpmcQueue *q, Request req);

// Неблокирующая вставка: возвращает 1 при успехе, 0 если очередь заполнена
int mpmc_queue_try_enqueue(MpmcQueue *q, Request req, Request **preq);

// Блокирующее извлечение (ждёт, пока не появятся данные)
Request mpmc_queue_dequeue(MpmcQueue *q);

// Неблокирующее извлечение: возвращает 1 и данные в *req, либо 0 (пусто)
int mpmc_queue_try_dequeue(MpmcQueue *q, Request *req);


#ifdef __cplusplus
}
#endif

#endif // FILE_LIBSRPC_MPMCQ_H


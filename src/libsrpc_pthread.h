#ifndef FILE_LIBSRPC_PTHREAD_H
#define FILE_LIBSRPC_PTHREAD_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*thread_func_t)(void *arg);

/*
 * Флаги:
 *
 * THREAD_FLAG_JOINABLE
 *      Создать поток joinable. Если задан этот флаг, thread_out обязан быть != NULL.
 *
 * THREAD_FLAG_DETACHED
 *      Создать поток detached.
 *
 * Если ни JOINABLE, ни DETACHED не заданы:
 *      thread_out != NULL -> joinable
 *      thread_out == NULL -> detached
 *
 * THREAD_FLAG_EXPLICIT_SCHED
 *      Использовать sched_policy/sched_priority из структуры.
 *      Если флаг не задан, планировщик наследуется от создавшего потока.
 *
 * THREAD_FLAG_NICE
 *      Применить nice_value внутри нового потока.
 *      Полезно для SCHED_OTHER. Для RT-политик nice обычно смысла не имеет.
 */
enum {
    THREAD_FLAG_JOINABLE       = 1u << 0,
    THREAD_FLAG_DETACHED       = 1u << 1,
    THREAD_FLAG_EXPLICIT_SCHED = 1u << 2,
    THREAD_FLAG_NICE           = 1u << 3
};

typedef struct thread_flag_s {
    uint32_t flags;

    /*
     * Если нужен joinable-поток, сюда нужно передать указатель на pthread_t.
     * Если NULL и поток не joinable, то поток будет создан detached.
     */
    pthread_t *thread_out;

    /*
     * Параметры планировщика.
     * Используются только если задан THREAD_FLAG_EXPLICIT_SCHED.
     */
    int sched_policy;    /* SCHED_OTHER, SCHED_FIFO, SCHED_RR, ... */
    int sched_priority;  /* для RT: от sched_get_priority_min до max */

    /*
     * nice используется только если задан THREAD_FLAG_NICE.
     * Обычный диапазон: -20..19.
     */
    int nice_value;

    /*
     * Пользовательский стек.
     *
     * Если stack_addr != NULL, то stack_size обязателен и должен быть
     * не меньше PTHREAD_STACK_MIN.
     *
     * Если stack_addr == NULL, но stack_size != 0, то задаётся только размер стека.
     */
    void *stack_addr;
    size_t stack_size;
    size_t guard_size;

    /*
     * CPU affinity.
     *
     * cpuset указывает на cpu_set_t.
     * Если cpuset_size == 0, используется sizeof(cpu_set_t).
     *
     * Если вы используете CPU_ALLOC/CPU_ALLOC_SIZE, задайте cpuset_size явно.
     */
    const void *cpuset;
    size_t cpuset_size;

    /*
     * Имя потока.
     * Копируется внутри pthread_start(), поэтому после вызова может быть освобождено.
     * Linux ограничивает имя потока примерно 15 символами.
     */
    const char *name;
} thread_flag_t;

static inline thread_flag_t thread_flag_default(void)
{
    thread_flag_t f = {0};
    return f;
}

/*
 * Возврат:
 *   0            успех
 *   -EINVAL      неверные аргументы
 *   -ENOMEM      не хватило памяти
 *   -rc          ошибка pthread_*(), где rc > 0 это номер errno
 */
int pthread_start(thread_func_t func, void *arg, thread_flag_t flags);
int pthread_start_all_cpu(thread_func_t func, void *arg);

#ifdef __cplusplus
}
#endif

#endif // FILE_LIBSRPC_PTHREAD_H

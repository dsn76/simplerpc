/* Qwen3.8-Max */

#define _GNU_SOURCE

#include "libsrpc_futex.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#include <sys/syscall.h>
#include <linux/futex.h>

#define JOB_FUTEX_MAGIC        0x4A465558u /* "JFUX" */
#define JOB_FUTEX_MAGIC_INIT   0x4A46494Eu /* "JFIN" */
#define JOB_FUTEX_VERSION      1u

#ifndef FUTEX_WAIT_BITSET
#define FUTEX_WAIT_BITSET 9
#endif

#ifndef FUTEX_BITSET_MATCH_ANY
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFu
#endif

#ifndef SYS_futex
#ifdef __NR_futex
#define SYS_futex __NR_futex
#else
#error "futex syscall number is not available"
#endif
#endif

/*
 * Используется для насыщения очень больших таймаутов.
 * Расчёт предполагает Linux user-space и signed time_t.
 */
#define JOB_FUTEX_TIME_T_MAX \
    ((time_t)(((uintmax_t)1 << (sizeof(time_t) * 8 - 1)) - 1))

_Static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");

static int check_pointer(job_futex_t *jf)
{
    if (jf == NULL)
        return -EFAULT;

    if (((uintptr_t)jf & (__alignof__(job_futex_t) - 1)) != 0)
        return -EINVAL;

    return 0;
}

static int check_object(job_futex_t *jf)
{
    int rc = check_pointer(jf);
    if (rc != 0)
        return rc;

    uint32_t magic = __atomic_load_n(&jf->magic, __ATOMIC_ACQUIRE);

    if (magic == JOB_FUTEX_MAGIC_INIT)
        return -EAGAIN;

    if (magic != JOB_FUTEX_MAGIC)
        return -EPROTO;

    uint32_t version = __atomic_load_n(&jf->version, __ATOMIC_ACQUIRE);
    if (version != JOB_FUTEX_VERSION)
        return -EPROTO;

    return 0;
}

static int state_to_client_err(uint32_t state)
{
    switch (state) {
    case JOB_FUTEX_STATE_DONE:
        return 0;
    case JOB_FUTEX_STATE_CANCELED:
        return -ECANCELED;
    case JOB_FUTEX_STATE_ERROR:
        return -EIO;
    case JOB_FUTEX_STATE_DESTROYED:
        return -ESHUTDOWN;
    case JOB_FUTEX_STATE_PENDING:
        return -EAGAIN;
    default:
        return -EPROTO;
    }
}

static int state_to_worker_err(uint32_t state)
{
    switch (state) {
    case JOB_FUTEX_STATE_PENDING:
        return 0;
    case JOB_FUTEX_STATE_DONE:
        return -EALREADY;
    case JOB_FUTEX_STATE_CANCELED:
        return -ECANCELED;
    case JOB_FUTEX_STATE_ERROR:
        return -EIO;
    case JOB_FUTEX_STATE_DESTROYED:
        return -ESHUTDOWN;
    default:
        return -EPROTO;
    }
}

static int timespec_ge(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec > b->tv_sec;

    return a->tv_nsec >= b->tv_nsec;
}

/*
 * Возвращает:
 *   0 - timeout бесконечный, deadline не нужен;
 *   1 - finite deadline успешно заполнен;
 *   <0 - ошибка.
 */
static int make_deadline(uint64_t timeout_us, struct timespec *deadline)
{
    if (timeout_us == JOB_FUTEX_INFINITE)
        return 0;

    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -errno;

    uint64_t add_sec  = timeout_us / 1000000ULL;
    uint64_t add_nsec = (timeout_us % 1000000ULL) * 1000ULL;

    uint64_t sec  = (uint64_t)now.tv_sec + add_sec;
    uint64_t nsec = (uint64_t)now.tv_nsec + add_nsec;

    sec += nsec / 1000000000ULL;
    nsec %= 1000000000ULL;

    uint64_t max_sec = (uint64_t)JOB_FUTEX_TIME_T_MAX;
    if (sec > max_sec)
        sec = max_sec;

    deadline->tv_sec  = (time_t)sec;
    deadline->tv_nsec = (long)nsec;

    return 1;
}

/*
 * Shared futex wait.
 *
 * Важно: используется FUTEX_WAIT_BITSET без PRIVATE-флага.
 * Это межпроцессный shared futex.
 *
 * timeout должен быть абсолютным по CLOCK_MONOTONIC.
 */
static int futex_wait_shared_bitset(const uint32_t *uaddr,
                                    uint32_t expected,
                                    const struct timespec *abs_timeout)
{
    long rc = syscall(SYS_futex,
                      uaddr,
                      FUTEX_WAIT_BITSET,
                      expected,
                      abs_timeout,
                      NULL,
                      FUTEX_BITSET_MATCH_ANY);

    if (rc < 0)
        return -errno;

    return 0;
}

static int futex_wake_shared(const uint32_t *uaddr, int count)
{
    long rc = syscall(SYS_futex,
                      uaddr,
                      FUTEX_WAKE,
                      count,
                      NULL,
                      NULL,
                      0);

    if (rc < 0)
        return -errno;

    return (int)rc;
}

int job_futex_init(job_futex_t *jf, uint32_t worker_count)
{
    int rc = check_pointer(jf);
    if (rc != 0)
        return rc;

    if (worker_count == 0)
        return -EINVAL;

    /*
     * Аккуратный вход в инициализацию.
     *
     * Для первого раза ожидается нулевая структура.
     * Для повторной инициализации разрешаем только состояние DESTROYED.
     */
    uint32_t expected_magic = 0;

    if (__atomic_compare_exchange_n(&jf->magic,
                                    &expected_magic,
                                    JOB_FUTEX_MAGIC_INIT,
                                    false,
                                    __ATOMIC_ACQUIRE,
                                    __ATOMIC_ACQUIRE)) {
        /*
         * Fresh zero-initialized object.
         */
    } else if (expected_magic == JOB_FUTEX_MAGIC) {
        uint32_t st = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

        if (st != JOB_FUTEX_STATE_DESTROYED)
            return -EBUSY;

        expected_magic = JOB_FUTEX_MAGIC;

        if (!__atomic_compare_exchange_n(&jf->magic,
                                         &expected_magic,
                                         JOB_FUTEX_MAGIC_INIT,
                                         false,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_ACQUIRE)) {
            return -EBUSY;
        }
    } else if (expected_magic == JOB_FUTEX_MAGIC_INIT) {
        return -EAGAIN;
    } else {
        return -EPROTO;
    }

    /*
     * Теперь magic == JOB_FUTEX_MAGIC_INIT.
     * Другие процессы, увидев его, получат -EAGAIN.
     */

    uint32_t old_generation = __atomic_load_n(&jf->generation, __ATOMIC_RELAXED);
    uint32_t new_generation = old_generation + 1;

    /*
     * Generation 0 считаем невалидным.
     */
    if (new_generation == 0)
        new_generation = 1;

    __atomic_store_n(&jf->version, JOB_FUTEX_VERSION, __ATOMIC_RELAXED);
    __atomic_store_n(&jf->worker_count, worker_count, __ATOMIC_RELAXED);
    __atomic_store_n(&jf->completed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&jf->generation, new_generation, __ATOMIC_RELAXED);

    jf->reserved[0] = 0;
    jf->reserved[1] = 0;

    /*
     * Публикуем состояние.
     *
     * State остаётся PENDING, пока все исполнители не завершат работу.
     * Именно на этом поле будет спать заказчик.
     */
    __atomic_store_n(&jf->state, JOB_FUTEX_STATE_PENDING, __ATOMIC_RELEASE);

    /*
     * Публикуем magic последним, чтобы check_object() видел уже
     * максимально подготовленный объект.
     */
    __atomic_store_n(&jf->magic, JOB_FUTEX_MAGIC, __ATOMIC_RELEASE);

    return 0;
}

int job_futex_destroy(job_futex_t *jf)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    uint32_t state = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

    if (state == JOB_FUTEX_STATE_DESTROYED)
        return 0;

    /*
     * Деструктор намеренно переводит объект в DESTROYED из любого состояния.
     * Память при этом не освобождается.
     */
    __atomic_store_n(&jf->state, JOB_FUTEX_STATE_DESTROYED, __ATOMIC_RELEASE);

    /*
     * Если кто-то ждёт на futex, даём ему проснуться и увидеть DESTROYED.
     */
    (void)futex_wake_shared(&jf->state, INT_MAX);

    return 0;
}

int job_futex_client_wait(job_futex_t *jf, uint64_t timeout_us)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    uint32_t worker_count = __atomic_load_n(&jf->worker_count, __ATOMIC_ACQUIRE);

    if (worker_count == 0)
        return -EPROTO;

    uint32_t state = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

    if (state == JOB_FUTEX_STATE_DONE)
        return 0;

    if (state != JOB_FUTEX_STATE_PENDING)
        return state_to_client_err(state);

    /*
     * timeout_us == 0: non-blocking.
     */
    if (timeout_us == 0)
        return -ETIMEDOUT;

    struct timespec deadline;
    int finite = 0;

    if (timeout_us != JOB_FUTEX_INFINITE) {
        rc = make_deadline(timeout_us, &deadline);
        if (rc < 0)
            return rc;

        finite = 1;
    }

    for (;;) {
        state = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

        if (state == JOB_FUTEX_STATE_DONE)
            return 0;

        if (state != JOB_FUTEX_STATE_PENDING)
            return state_to_client_err(state);

        if (finite) {
            struct timespec now;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                return -errno;

            if (timespec_ge(&now, &deadline))
                return -ETIMEDOUT;
        }

        /*
         * Ждём, только если state всё ещё PENDING.
         *
         * Если состояние изменилось между нашей проверкой и входом в ядро,
         * futex вернёт -EAGAIN, и мы повторим проверку.
         *
         * Таким образом lost wakeup исключается.
         */
        rc = futex_wait_shared_bitset(&jf->state,
                                      JOB_FUTEX_STATE_PENDING,
                                      finite ? &deadline : NULL);

        if (rc == 0 || rc == -EAGAIN || rc == -EINTR)
            continue;

        if (rc == -ETIMEDOUT)
            return -ETIMEDOUT;

        return rc;
    }
}

static int worker_done_impl(job_futex_t *jf, uint32_t generation, int check_generation)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    if (check_generation) {
        uint32_t current_generation =
            __atomic_load_n(&jf->generation, __ATOMIC_ACQUIRE);

        if (current_generation != generation)
            return -ESTALE;
    }

    uint32_t state = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

    if (state != JOB_FUTEX_STATE_PENDING)
        return state_to_worker_err(state);

    uint32_t worker_count = __atomic_load_n(&jf->worker_count, __ATOMIC_ACQUIRE);

    if (worker_count == 0)
        return -EPROTO;

    uint32_t old_completed = __atomic_load_n(&jf->completed, __ATOMIC_ACQUIRE);

    for (;;) {
        if (old_completed >= worker_count)
            return -EALREADY;

        uint32_t desired_completed = old_completed + 1;

        if (__atomic_compare_exchange_n(&jf->completed,
                                        &old_completed,
                                        desired_completed,
                                        false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (desired_completed != worker_count)
                return 0;

            /*
             * Мы последний.
             *
             * Переводим state PENDING -> DONE.
             *
             * Release-запись важна: все данные, которые исполнители
             * записали в shared memory до вызова job_futex_worker_done(),
             * должны стать видимыми заказчику после acquire-чтения state.
             */
            uint32_t expected_state = JOB_FUTEX_STATE_PENDING;

            if (__atomic_compare_exchange_n(&jf->state,
                                            &expected_state,
                                            JOB_FUTEX_STATE_DONE,
                                            false,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_ACQUIRE)) {
                /*
                 * Будим заказчика.
                 *
                 * Обычно заказчик один, но INT_MAX безопаснее на случай,
                 * если приложение когда-нибудь решит ждать несколькими
                 * потоками.
                 */
                (void)futex_wake_shared(&jf->state, INT_MAX);
                return 0;
            }

            /*
             * Кто-то успел перевести запрос в CANCELED/ERROR/DESTROYED.
             * Не переопределяем терминальное состояние.
             */
            return state_to_worker_err(expected_state);
        }

        /*
         * CAS не удался, old_completed обновлён самим компилятором/GCC.
         * Повторяем.
         */
    }
}

int job_futex_worker_done(job_futex_t *jf)
{
    return worker_done_impl(jf, 0, 0);
}

int job_futex_worker_done_gen(job_futex_t *jf, uint32_t generation)
{
    return worker_done_impl(jf, generation, 1);
}

static int transition_from_pending(job_futex_t *jf, uint32_t new_state)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    uint32_t expected_state = JOB_FUTEX_STATE_PENDING;

    if (__atomic_compare_exchange_n(&jf->state,
                                    &expected_state,
                                    new_state,
                                    false,
                                    __ATOMIC_RELEASE,
                                    __ATOMIC_ACQUIRE)) {
        (void)futex_wake_shared(&jf->state, INT_MAX);
        return 0;
    }

    if (expected_state == new_state)
        return 0;

    return state_to_worker_err(expected_state);
}

int job_futex_cancel(job_futex_t *jf)
{
    return transition_from_pending(jf, JOB_FUTEX_STATE_CANCELED);
}

int job_futex_mark_error(job_futex_t *jf)
{
    return transition_from_pending(jf, JOB_FUTEX_STATE_ERROR);
}

int job_futex_get_state(job_futex_t *jf, uint32_t *state)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    if (state == NULL)
        return -EINVAL;

    *state = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

    return 0;
}

int job_futex_get_completed(job_futex_t *jf, uint32_t *completed)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    if (completed == NULL)
        return -EINVAL;

    *completed = __atomic_load_n(&jf->completed, __ATOMIC_ACQUIRE);

    return 0;
}

int job_futex_get_generation(job_futex_t *jf, uint32_t *generation)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    if (generation == NULL)
        return -EINVAL;

    *generation = __atomic_load_n(&jf->generation, __ATOMIC_ACQUIRE);

    return 0;
}

int job_futex_is_done(job_futex_t *jf, int *done)
{
    int rc = check_object(jf);
    if (rc != 0)
        return rc;

    if (done == NULL)
        return -EINVAL;

    uint32_t state = __atomic_load_n(&jf->state, __ATOMIC_ACQUIRE);

    *done = (state == JOB_FUTEX_STATE_DONE);

    return 0;
}

const char *job_futex_strerror(int rc)
{
    if (rc == 0)
        return "Success";

    if (rc < 0)
        return strerror(-rc);

    return "Unknown error";
}


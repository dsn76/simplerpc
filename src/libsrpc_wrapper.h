#ifndef FILE_LIBSRPC_WRAPPER_H
#define FILE_LIBSRPC_WRAPPER_H

#include <semaphore.h>
#include <time.h>

#include "libsrpc_types.h"
#include "libsrpc_errno.h"

typedef sem_t libsrpc_sem_t;

/** MiMo V2.5 Free:
 * ВНИМАНИЕ: sem_init с pshared=1 работает только на Linux (glibc).
 * На других POSIX-системах sem_t может быть не relocatable в shared memory.
 * Для переносимости рекомендуется реализовать семафор через futex.
 */
static inline int libsrpc_sem_init(libsrpc_sem_t *sem, unsigned int value)
{
    return sem_init(sem, 1, value);
}

static inline int libsrpc_sem_destroy(libsrpc_sem_t *sem)
{
    return sem_destroy(sem);
}

static inline int libsrpc_sem_getvalue(libsrpc_sem_t *sem, int *sval)
{
    return sem_getvalue(sem, sval);
}

static inline int libsrpc_sem_wait(libsrpc_sem_t *sem)
{
    return sem_wait(sem);
}

static inline int libsrpc_sem_post(libsrpc_sem_t *sem)
{
    return sem_post(sem);
}

static inline int libsrpc_sem_trywait(libsrpc_sem_t *sem)
{
    return sem_trywait(sem);
}

static inline int libsrpc_sem_timedwait(libsrpc_sem_t *sem, const struct timespec *abs_timeout)
{
    return sem_timedwait(sem, abs_timeout);
}

/* timeout в микросекундах. */
static inline int libsrpc_sem_wait_timeout_us(libsrpc_sem_t *sem, uint64_t timeout)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout / 1000000ULL;
    ts.tv_nsec += (timeout % 1000000ULL) * 1000ULL;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return sem_timedwait(sem, &ts);
}


#endif // FILE_LIBSRPC_WRAPPER_H

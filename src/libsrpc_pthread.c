/* Qwen pthread implementation */
#define _GNU_SOURCE

#include "libsrpc_pthread.h"
#include "libsrpc_debug_print.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/resource.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN (16384UL)
#endif

typedef struct start_ctx_s {
    thread_func_t func;
    void *arg;
    char *name;
    int set_nice;
    int nice_value;
} start_ctx_t;

/*
 * Эта функция запускается pthread_create().
 * Она устанавливает имя и nice, после чего вызывает пользовательскую функцию.
 */
static void *start_thunk(void *arg)
{
    start_ctx_t ctx = *(start_ctx_t *)arg;
    free(arg);

    if (ctx.name != NULL) {
        /*
         * pthread_setname_np() в Linux обычно принимает не более 15 байт
         * имени плюс завершающий NUL.
         */
        char comm[16];
        (void)snprintf(comm, sizeof(comm), "%s", ctx.name);
        (void)pthread_setname_np(pthread_self(), comm);
        free(ctx.name);
    }

    if (ctx.set_nice) {
        errno = 0;
        /*
         * В Linux это влияет на текущий поток/задачу.
         * Для отрицательных nice обычно нужны права или cap_sys_nice.
         */
        (void)setpriority(PRIO_PROCESS, 0, ctx.nice_value);
    }

    return ctx.func(ctx.arg);
}

int pthread_start(thread_func_t func, void *arg, thread_flag_t flags)
{
    if (func == NULL) {
        ERR_PRINT("func is NULL\n");
        return -EINVAL;
    }

    if ((flags.flags & THREAD_FLAG_NICE) &&
        (flags.nice_value < -20 || flags.nice_value > 19)) {
        ERR_PRINT("nice value is out of range\n");
        return -EINVAL;
    }

    int wants_join = (flags.flags & THREAD_FLAG_JOINABLE) != 0;
    int wants_detach = (flags.flags & THREAD_FLAG_DETACHED) != 0;

    if (wants_join && wants_detach) {
        ERR_PRINT("wants_join and wants_detach are set\n");
        return -EINVAL;
    }

    if (wants_join && flags.thread_out == NULL) {
        /*
         * Для joinable-потока нужно куда-то вернуть pthread_t,
         * иначе pthread_join() будет невозможен.
         */
        ERR_PRINT("wants_join and thread_out is NULL\n");
        return -EINVAL;
    }

    int detached;

    if (wants_detach) {
        detached = 1;
    } else if (wants_join) {
        detached = 0;
    } else {
        /*
         * Если пользователь явно не указал JOINABLE/DETACHED:
         * - при наличии thread_out делаем joinable;
         * - при thread_out == NULL делаем detached, чтобы не терять ресурсы.
         */
        detached = (flags.thread_out == NULL);
    }

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        ERR_PRINT("pthread_attr_init failed\n");
        return -rc;
    }

    rc = pthread_attr_setdetachstate(
        &attr,
        detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE
    );
    if (rc != 0) {
        ERR_PRINT("pthread_attr_setdetachstate failed\n");
        goto out;
    }

    if (flags.flags & THREAD_FLAG_EXPLICIT_SCHED) {
        int prio_min = sched_get_priority_min(flags.sched_policy);
        int prio_max = sched_get_priority_max(flags.sched_policy);

        if (prio_min < 0 || prio_max < 0 ||
            flags.sched_priority < prio_min ||
            flags.sched_priority > prio_max) {
            rc = EINVAL;
            ERR_PRINT("sched_get_priority_min or sched_get_priority_max failed\n");
            goto out;
        }

        rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        if (rc != 0) {
            ERR_PRINT("pthread_attr_setinheritsched failed\n");
            goto out;
        }

        rc = pthread_attr_setschedpolicy(&attr, flags.sched_policy);
        if (rc != 0) {
            ERR_PRINT("pthread_attr_setschedpolicy failed\n");
            goto out;
        }

        struct sched_param sp = {
            .sched_priority = flags.sched_priority,
        };

        rc = pthread_attr_setschedparam(&attr, &sp);
        if (rc != 0) {
            ERR_PRINT("pthread_attr_setschedparam failed\n");
            goto out;
        }
    }

    if (flags.stack_addr != NULL || flags.stack_size != 0) {
        if (flags.stack_size < (size_t)PTHREAD_STACK_MIN) {
            rc = EINVAL;
            ERR_PRINT("stack size is less than PTHREAD_STACK_MIN\n");
            goto out;
        }

        if (flags.stack_addr != NULL) {
            rc = pthread_attr_setstack(&attr, flags.stack_addr, flags.stack_size);
        } else {
            rc = pthread_attr_setstacksize(&attr, flags.stack_size);
        }

        if (rc != 0) {
            ERR_PRINT("pthread_attr_setstack or pthread_attr_setstacksize failed\n");
            goto out;
        }
    }

    if (flags.guard_size != 0) {
        rc = pthread_attr_setguardsize(&attr, flags.guard_size);
        if (rc != 0) {
            ERR_PRINT("pthread_attr_setguardsize failed\n");
            goto out;
        }
    }

    if (flags.cpuset != NULL) {
        size_t sz = (flags.cpuset_size != 0)
                        ? flags.cpuset_size
                        : sizeof(cpu_set_t);

        rc = pthread_attr_setaffinity_np(&attr, sz,  (const cpu_set_t *)flags.cpuset );
        if (rc != 0) {
            ERR_PRINT("pthread_attr_setaffinity_np failed sz=%zu/%zu rc=%d err=%s\n", sz, sizeof(cpu_set_t), rc, strerror(rc));
            goto out;
        }
    }

    char *name_copy = NULL;

    if (flags.name != NULL) {
        name_copy = strdup(flags.name);
        if (name_copy == NULL) {
            rc = ENOMEM;
            ERR_PRINT("strdup failed\n");
            goto out;
        }
    }

    start_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        free(name_copy);
        rc = ENOMEM;
        ERR_PRINT("malloc failed\n");
        goto out;
    }

    ctx->func = func;
    ctx->arg = arg;
    ctx->name = name_copy;
    ctx->set_nice = (flags.flags & THREAD_FLAG_NICE) ? 1 : 0;
    ctx->nice_value = flags.nice_value;

    pthread_t tid;

    rc = pthread_create(&tid, &attr, start_thunk, ctx);
    if (rc != 0) {
        free(name_copy);
        free(ctx);
        ERR_PRINT("pthread_create failed\n");
        goto out;
    }

    if (flags.thread_out != NULL) {
        *flags.thread_out = tid;
    }

    rc = 0;
out:
    pthread_attr_destroy(&attr);
    return rc != 0 ? rc : 0;
}

int pthread_start_all_cpu(thread_func_t func, void *arg)
{
    int rc = 0;
    unsigned int cpu = 0;
    cpu_set_t mask = {0};
    thread_flag_t flags = thread_flag_default();

    flags.flags |= THREAD_FLAG_DETACHED;

    rc = sched_getaffinity(0, sizeof(mask), &mask);
    if (rc != 0) {
        ERR_PRINT("sched_getaffinity failed\n");
        goto err;
    }

    int n = CPU_COUNT(&mask);
    if (n <= 0) {
        ERR_PRINT("CPU_COUNT failed\n");
        goto err;
    }

    for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &mask)) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            flags.cpuset = &set;
            flags.cpuset_size = sizeof(set);
            
            rc = pthread_start(func, arg, flags);
            if (rc != 0) {
                ERR_PRINT("pthread_start failed CPU=%u  rc=%d\n", cpu, rc);
                goto err;
            }
        }
    }

out:
    return(rc);
err:
    goto out;
}


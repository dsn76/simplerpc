/* Замер задержки RPC-вызова calc_add() к внешнему процессу-экспортёру.
 *
 * Этапы:
 *   1) локальный вызов функции того же профиля (базовая линия);
 *   2) glibc malloc/free 64 байта;
 *   3) libsrpc_shmem_malloc/free 64 байта в разделяемом пуле;
 *   4) прогрев RPC-канала;
 *   5) одиночные запросы с паузой 0.01 с: min/макс/среднее время;
 *   6) полный round-trip RPC в одном потоке;
 *   7) round-trip RPC из нескольких потоков одновременно (пропускная способность).
 *
 * Использование: bench_srpc_client [итераций] [потоков] [итераций_на_поток]
 */

#include "bench_common.h"

#include <pthread.h>
#include <unistd.h>

#include "libsrpc.h"
#include "libsrpc_errno.h"

__attribute__((noinline))
static int local_add(int a, int b)
{
    return a + b;
}

static int (*volatile local_add_p)(int, int) = local_add;

typedef struct {
    size_t iters;
    uint64_t *samples;
    size_t done;
    int failed;
} thread_ctx_t;

static void *thread_body(void *arg)
{
    thread_ctx_t *ctx = arg;

    for (size_t i = 0; i < ctx->iters; i++) {
        uint64_t t0 = bench_now_ns();
        int r = calc_add(1, 2);
        uint64_t t1 = bench_now_ns();

        if (r != 3) {
            ctx->failed = 1;
            break;
        }
        ctx->samples[i] = t1 - t0;
        ctx->done++;
    }
    return NULL;
}

int main(int ac, char **av)
{
    size_t iters = (ac > 1) ? (size_t)strtoul(av[1], NULL, 10) : 4000;
    size_t nthreads = (ac > 2) ? (size_t)strtoul(av[2], NULL, 10) : 4;
    size_t titers = (ac > 3) ? (size_t)strtoul(av[3], NULL, 10) : 500;
    if (iters == 0 || iters > 10000000) iters = 4000;
    if (nthreads == 0 || nthreads > 256) nthreads = 4;
    if (titers == 0 || titers > 10000000) titers = 500;

    uint64_t *s = calloc(iters, sizeof(*s));
    if (!s) return 1;

    printf("bench_srpc_client: pid=%d итераций=%zu потоков=%zu×%zu\n",
           getpid(), iters, nthreads, titers);

    local_add_p(1, 2); // прогрев

    /* 1. Локальный вызов той же арности. */
    for (size_t i = 0; i < iters; i++) {
        uint64_t t0 = bench_now_ns();
        int r = local_add_p(1, 2);
        uint64_t t1 = bench_now_ns();
        if (r != 3) return 1;
        s[i] = t1 - t0;
    }
    bench_report("локальный вызов", s, iters, 0);

    /* 2. Штатный аллокатор процесса. */
    for (size_t i = 0; i < iters; i++) {
        uint64_t t0 = bench_now_ns();
        void *p = malloc(64);
        free(p);
        uint64_t t1 = bench_now_ns();
        s[i] = t1 - t0;
    }
    bench_report("glibc malloc+free 64Б", s, iters, 0);

    /* 3. Аллокатор разделяемого пула: robust mutex + TLSF + undo-log. */
    for (size_t i = 0; i < iters; i++) {
        uint64_t t0 = bench_now_ns();
        void *p = libsrpc_shmem_malloc(64);
        libsrpc_shmem_free(p);
        uint64_t t1 = bench_now_ns();
        s[i] = t1 - t0;
    }
    bench_report("shmem malloc+free 64Б", s, iters, 0);

    /* 4. Прогрев RPC-канала. */
    for (int i = 0; i < 100; i++) {
        if (calc_add(1, 2) != 3) {
            printf("ОШИБКА: экспортёр calc_add не найден (errno=%d %s)\n",
                   libsrpc_errno, libsrpc_strerror(libsrpc_errno));
            return 1;
        }
    }

    /* 5. Одиночные запросы с паузой 0.01 с (низкая нагрузка). */
    size_t nslow = 100;
    uint64_t *slow = calloc(nslow, sizeof(*slow));
    if (!slow) return 1;

    size_t okslow = 0;
    for (size_t i = 0; i < nslow; i++) {
        usleep(10 * 1000);
        uint64_t t0 = bench_now_ns();
        int r = calc_add(1, 2);
        uint64_t t1 = bench_now_ns();
        if (r != 3) {
            printf("ВНИМАНИЕ: запрос %zu вернул %d (errno=%d %s) — замер прерван\n",
                   i, r, libsrpc_errno, libsrpc_strerror(libsrpc_errno));
            break;
        }
        slow[i] = t1 - t0;
        okslow++;
    }
    bench_report("sRPC round-trip пауза 0.01с", slow, okslow, 0);
    free(slow);

    /* 6. Однопоточный round-trip RPC. */
    size_t ok = 0;
    uint64_t wall0 = bench_now_ns();
    for (size_t i = 0; i < iters; i++) {
        uint64_t t0 = bench_now_ns();
        int r = calc_add(1, 2);
        uint64_t t1 = bench_now_ns();
        if (r != 3) {
            printf("ВНИМАНИЕ: вызов %zu вернул %d (errno=%d %s) — замер прерван\n",
                   i, r, libsrpc_errno, libsrpc_strerror(libsrpc_errno));
            break;
        }
        s[i] = t1 - t0;
        ok++;
    }
    uint64_t wall1 = bench_now_ns();
    bench_report("sRPC round-trip 1 поток ", s, ok, wall1 - wall0);

    /* 7. Многопоточная нагрузка на одного экспортёра. */
    pthread_t *tid = calloc(nthreads, sizeof(*tid));
    thread_ctx_t *ctx = calloc(nthreads, sizeof(*ctx));
    if (!tid || !ctx) return 1;

    for (size_t t = 0; t < nthreads; t++) {
        ctx[t].iters = titers;
        ctx[t].samples = calloc(titers, sizeof(uint64_t));
        if (!ctx[t].samples) return 1;
    }

    wall0 = bench_now_ns();
    for (size_t t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, thread_body, &ctx[t]);
    for (size_t t = 0; t < nthreads; t++)
        pthread_join(tid[t], NULL);
    wall1 = bench_now_ns();

    size_t total = 0;
    for (size_t t = 0; t < nthreads; t++) total += ctx[t].done;

    uint64_t *all = calloc(total ? total : 1, sizeof(uint64_t));
    if (!all) return 1;
    size_t k = 0;
    for (size_t t = 0; t < nthreads; t++)
        for (size_t i = 0; i < ctx[t].done; i++) all[k++] = ctx[t].samples[i];

    char label[64];
    snprintf(label, sizeof(label), "sRPC round-trip %zu потока", nthreads);
    bench_report(label, all, total, wall1 - wall0);

    printf("bench_srpc_client: успешных RPC=%zu\n", ok + total);
    free(s);
    free(all);
    free(tid);
    for (size_t t = 0; t < nthreads; t++) free(ctx[t].samples);
    free(ctx);
    return 0;
}

#ifndef FILE_BENCH_COMMON_H
#define FILE_BENCH_COMMON_H

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int bench_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Печатает распределение задержек одного этапа замера.
 * samples портится (сортируется на месте).
 */
static inline void bench_report(const char *name, uint64_t *samples, size_t n,
                                uint64_t wall_ns)
{
    if (n == 0) {
        printf("%-28s нет замеров\n", name);
        return;
    }

    qsort(samples, n, sizeof(*samples), bench_cmp_u64);

    long double sum = 0.0L;
    for (size_t i = 0; i < n; i++) sum += (long double)samples[i];

    double mean = (double)(sum / (long double)n);
    double p50 = (double)samples[n / 2];
    double p90 = (double)samples[(size_t)(n * 0.90)];
    double p99 = (double)samples[(size_t)(n * 0.99)];

    printf("%-28s n=%-7zu min=%8.3f;  p50=%8.3f;  p90=%8.3f;  p99=%9.3f;  max=%10.3f;  "
           "mean=%8.3f; мкс",
           name, n,
           samples[0] / 1000.0, p50 / 1000.0, p90 / 1000.0,
           p99 / 1000.0, samples[n - 1] / 1000.0, mean / 1000.0);

    if (wall_ns > 0) {
        double rate = (double)n / ((double)wall_ns / 1e9);
        printf("  %.0f оп/с", rate);
    }
    printf("\n");
    fflush(stdout);
}

#endif /* FILE_BENCH_COMMON_H */

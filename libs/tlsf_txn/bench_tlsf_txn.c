/*
 * bench_tlsf_txn.c — микробенчмарк TLSF v4.1 (malloc/free и lock-free dc)
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "tlsf_txn.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#define POOL   (1024 * 1024)
#define ITERS  200000

static double ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double ns_per(double start, int n)
{
    return (ns_now() - start) / (double)n;
}

int main(void)
{
    void *mem = mmap(NULL, POOL, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    tlsf_t t = tlsf_create(mem, POOL);
    if (!t) {
        fprintf(stderr, "tlsf_create failed\n");
        return 1;
    }

    void *p = tlsf_malloc(t, 128, 1, 7);
    if (!p) {
        fprintf(stderr, "warmup malloc failed\n");
        return 1;
    }

    double s = ns_now();
    for (int i = 0; i < ITERS; i++)
        tlsf_setdc(t, p);
    double setdc_ns = ns_per(s, ITERS);

    s = ns_now();
    for (int i = 0; i < ITERS; i++)
        (void)tlsf_getdc(t, p);
    double getdc_ns = ns_per(s, ITERS);

    s = ns_now();
    for (int i = 0; i < ITERS; i++)
        (void)tlsf_get_data_type(t, p);
    double gettype_ns = ns_per(s, ITERS);

    s = ns_now();
    for (int i = 0; i < ITERS; i++)
        tlsf_cleardc(t, p);
    double cleardc_ns = ns_per(s, ITERS);

    tlsf_free(t, p, 1);

    void *slots[256];
    memset(slots, 0, sizeof(slots));
    s = ns_now();
    for (int i = 0; i < ITERS; i++) {
        int idx = i & 255;
        if (slots[idx]) {
            tlsf_free(t, slots[idx], 1);
            slots[idx] = NULL;
        } else {
            slots[idx] = tlsf_malloc(t, 64 + (size_t)(i & 63), 1, 0);
        }
    }
    double mix_ns = ns_per(s, ITERS);
    for (int i = 0; i < 256; i++) {
        if (slots[i]) tlsf_free(t, slots[i], 1);
    }

    printf("TLSF v4.1 benchmark (%d iterations, pool %d KiB)\n", ITERS, POOL / 1024);
    printf("  tlsf_setdc          %8.1f ns/op  (spec < 100 ns)\n", setdc_ns);
    printf("  tlsf_cleardc        %8.1f ns/op  (spec < 100 ns)\n", cleardc_ns);
    printf("  tlsf_getdc          %8.1f ns/op  (spec < 50 ns)\n", getdc_ns);
    printf("  tlsf_get_data_type  %8.1f ns/op  (spec < 50 ns)\n", gettype_ns);
    printf("  malloc/free mix     %8.1f ns/op\n", mix_ns);

    tlsf_destroy(t);
    munmap(mem, POOL);
    return 0;
}

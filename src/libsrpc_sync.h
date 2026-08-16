#ifndef FILE_LIBSRPC_SYNC_H
#define FILE_LIBSRPC_SYNC_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <linux/futex.h>


typedef struct libsrpc_futex_s { 
    atomic_int val;
} libsrpc_futex_t __attribute__((aligned(4)));


// Системный вызов futex
/* Ожидание на futex-адресе, пока значение не изменится */
static inline int libsrpc_futex_wait(libsrpc_futex_t *futex, int val) {
    return syscall(SYS_futex, &futex->val, FUTEX_WAIT, val, NULL, NULL, 0);
}

/* Пробуждение num потоков, ожидающих на этом futex */
static inline int libsrpc_futex_wake(libsrpc_futex_t *futex, int num) {
    return syscall(SYS_futex, &futex->val, FUTEX_WAKE, num, NULL, NULL, 0);
}



#endif // FILE_LIBSRPC_SYNC_H


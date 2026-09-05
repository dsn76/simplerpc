#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

#include "libsrpc.h"


static void print_log(const char* msg) {
    char *str = libsrpc_shmem_malloc(1024);
    if(str) {
        snprintf(str, 1024, "%s", msg);
        log_write(str);
    }
    libsrpc_shmem_free(str);
}

int calc_add(int a, int b) {
    return a + b;
}

atomic_int_least32_t exit_flag = 0;

void all_exit(void) {
    atomic_store_explicit(&exit_flag, 1, memory_order_release);
}

int main(int ac, char **av)
{
    usleep(1000);

    print_log("CACL START");

    while (!atomic_load_explicit(&exit_flag, memory_order_acquire)) {
        usleep(1000);
    }

    print_log("CALC END");

    return 0;
}


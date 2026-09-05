#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

#include "libsrpc.h"

int sched_getcpu(void);

pid_t log_write(const char* msg) {
    fprintf(stderr, "LOG(%p)[%02d]: '%s'\n", (void*)msg, sched_getcpu(), msg);
    return(getpid());
}

atomic_int_least32_t exit_flag = 0;

void all_exit(void) {
    atomic_store_explicit(&exit_flag, 1, memory_order_release);
}


int main(int ac, char **av)
{
    usleep(1000);

    log_write("LOG START");

    while (!atomic_load_explicit(&exit_flag, memory_order_acquire)) {
        usleep(1000);
    }

    log_write("LOG END");
    usleep(1000);
    return 0;
}

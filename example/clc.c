#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

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


int main(int ac, char **av) {
    print_log("CACL START");
    sleep(60);
    print_log("CALC END");
    return 0;
}


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libsrpc.h"

int sched_getcpu(void);

void log_write(const char* msg) {
    fprintf(stderr, "LOG(%p)[%02d]: '%s'\n", (void*)msg, sched_getcpu(), msg);
}


int main(int ac, char **av) {
    log_write("LOG START");
    sleep(60);
    log_write("LOG END");
    return 0;
}


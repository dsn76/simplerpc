#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "../src/libsrpc.h"

void log_write(const char* msg) {
    fprintf(stderr, "LOG(%p): '%s'\n", (void*)msg, msg);
}


int main(int ac, char **av) {
    log_write("LOG START");
    sleep(60);
    log_write("LOG END");
    return 0;
}


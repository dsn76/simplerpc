
#include <stdio.h>
#include <unistd.h>
#include "libsrpc_debug_print.h"

#include "libsrpc.h"


int testlocal(void)
{
    printf("RPC call: LOCAL\n");
    return 0;
}


int main(int ac, char *av[])
{
    char *str = NULL;
    //sleep(1);
    printf("!!! start app !!!\n");

    int rc = testrpc0();
    printf("testrpc0: %d\n", rc);
    testrpc1(1,"Hello",3);
    testlocal();

    str = libsrpc_shmem_malloc(1024);
    printf("str=%p\n", str);
    if(str) {
        snprintf(str, 1024, "Hello, world, from %s! my pid=%d", av[0], getpid());
        log_write(str);
    }

    int result = calc_add(1, 2);
    printf("calc_add: %d\n", result);

    printf("!!! end app !!!\n");
    //sleep(1);
    libsrpc_shmem_free(str);
    return 0;
}


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
    printf("!!! start app !!!\n");

    testlocal();

    str = libsrpc_shmem_malloc(1024);
    printf("MALLOC str=%p\n", str);
    if(str) {
        snprintf(str, 1024, "Hello, world, from %s! my pid=%d", av[0], getpid());
        pid_t pid = log_write(str);
        printf("log_write() %d :: ", pid); /* Вывод первого результата выполнения функции log_write() */
        for(int i = 0, num = libsrpc_lastreq_num(); i < num; i++) {
            int rc = libsrpc_lastreq_get(i, &pid, sizeof(pid));
            if(!rc) {
                printf("pid[%d]=%d  ", i, pid); /* Вывод всех результатов выполнения функции log_write() */
            }else{
                printf("rc=%d '%s'  ", rc, libsrpc_strerror(-rc));
                break;
            }
        }
        printf("\n");
    }

    int result = calc_add(1, 2);
    printf("calc_add() %d\n", result);

    printf("!!! end app !!!\n");
    libsrpc_shmem_free(str);
    return 0;
}

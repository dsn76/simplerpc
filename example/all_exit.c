
#include <stdio.h>
#include <unistd.h>

#include "libsrpc.h"



int main(int ac, char *av[])
{
    printf("!!! start all_exit !!!\n");

    all_exit();

    printf("!!! end all_exit !!!\n");
    return 0;
}

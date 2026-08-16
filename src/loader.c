
#define _GNU_SOURCE

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dlfcn.h>

typedef int (*daemon_fn_t)(int ac, char *av[]);

int main(int ac, char *av[])
{
    int ret;
    const char* libpath = getenv("LIBSIMPLERPC_SO");

    if (!libpath) return 1;

    void* h = dlopen(libpath, RTLD_NOW);

    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    daemon_fn_t fn = (daemon_fn_t)dlsym( h, "simplerpc_daemon_main");

    if (!fn) return 1;

    ret = fn(ac, av);

    return ret;
}

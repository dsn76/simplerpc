
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "libsrpc_errno.h"

typedef struct errno2str_s {
    int err;
    const char *str;
} errno2str_t;

static errno2str_t errno2str[] = {
#define XF(ecod,estr)   [ecod - ELIBSRPC_START] = { .err = ecod, .str = estr },
    SRPC_ERRNO_LIST
#undef XF
};


__thread int __libsrpc_errno;  /* локальная для потока переменная */

int *
__libsrpc_errno_location (void)
{
    return &__libsrpc_errno;
}

void __libsrpc_errno_clear(void)
{
    __libsrpc_errno = 0;
}

void __libsrpc_errno_set(int errnum)
{
    __libsrpc_errno = errnum;
}

int libsrpc_errno_get(void)
{
    return __libsrpc_errno;
}

const char*  libsrpc_strerror(int errnum)
{
    const char *unk = "Unknown error code";
    if(errnum <= ELIBSRPC_START) return(strerror(errnum));
    if(errnum >= LIBSRPC_ERRNO_MAX) return(unk);
    return(errno2str[errnum - ELIBSRPC_START].str);
}


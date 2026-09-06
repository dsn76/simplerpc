#ifndef FILE_LIBSRPC_ERRNO_H
#define FILE_LIBSRPC_ERRNO_H

#include <errno.h>
// MAX_ERRNO
#ifndef MAX_ERRNO
#define MAX_ERRNO 4095
#endif

#define SRPC_ERRNO_LIST    \
    XF(ELIBNOINIT,"libsrpc no init")  \
	XF(ESHMNOINIT,"shared memory no init")  \
	XF(ERPCDISABLE,"rpc disabled")  \
	XF(ENOREGFUN,"no registered function")  \
    XF(ERECURSIVE,"Recursive RPC calls are prohibited")  \
    XF(ECANCELLED,"RPC call cancelled")  \
    XF(ENOTAVAILABLE,"Not available")  \
    XF(ERPCWAITIMEDOUT,"RPC wait timeout")  \


typedef enum {
	ELIBSRPC_START = MAX_ERRNO,
    #define XF(err,str)   err,
        SRPC_ERRNO_LIST
    #undef XF
    LIBSRPC_ERRNO_MAX
} libsrpc_errno_codes_t;

#define libsrpc_errno (*__libsrpc_errno_location ())

int * __libsrpc_errno_location (void);
void __libsrpc_errno_clear(void);
void __libsrpc_errno_set(int errnum);
int libsrpc_errno_get(void);
const char*  libsrpc_strerror(int errnum);

#endif // FILE_LIBSRPC_ERRNO_H

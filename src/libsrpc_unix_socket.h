#ifndef FILE_LIBSRPC_UNIX_SOCKET_H
#define FILE_LIBSRPC_UNIX_SOCKET_H

#include <stdint.h>

#include "libsrpc_futex.h"

struct epoll_event;
typedef struct libsrpc_epoll_s libsrpc_epoll_t;
typedef struct libsrpc_server_s libsrpc_server_t;
typedef struct srpc_pool_fba_s srpc_pool_fba_t;

typedef int (*libsrpc_srv_func_t)(libsrpc_server_t *srv, libsrpc_epoll_t *epoll_data, struct epoll_event *event);

typedef struct libsrpc_epoll_s {
    int socket;
    int epfd;
    uint32_t events;
    libsrpc_srv_func_t func;
    pid_t pid;
} libsrpc_epoll_t;

typedef struct libsrpc_server_s {
    int socket;
    int epfd;
    libsrpc_epoll_t epsrv;
    srpc_pool_fba_t *pool_epcln;
    int *pshm_fd;
    int clients;
    uint8_t init;
} libsrpc_server_t;


typedef struct libsrpc_client_s {
    int socket;
    int epfd;
    int shm_fd;
    int efd_exit; // eventfd for exit
    const char *sck_name;
    pid_t pid;
    job_futex_t jf; // futex для синхронизации состояния.
} libsrpc_client_t;

int libsrpc_unix_server_init(libsrpc_server_t *srv, const char *name);
int libsrpc_unix_server_exit(libsrpc_server_t *srv);
int libsrpc_unix_client_init(libsrpc_client_t *cli, const char *name);
int libsrpc_unix_client_exit(libsrpc_client_t *cli);
int libsrpc_unix_server_epoll_wait(libsrpc_server_t *srv, int timeout);

#endif // FILE_LIBSRPC_UNIX_SOCKET_H


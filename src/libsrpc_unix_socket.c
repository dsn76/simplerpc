
#define _GNU_SOURCE

#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/eventfd.h>

#include "libsrpc_local.h"
#include "libsrpc_fixblockalloc.h"
#include "libsrpc_pthread.h"
#include "libsrpc_unix_socket.h"
#include "libsrpc_proc.h"

/* ============================================================================== */
/* Реестр функций */
typedef struct regfn_msg_s {
    uint32_t        size; // Размер сообщения.
    char            sign[32]; // Сигнатура.
    srpc_bmp_func_t bmp; // Битовая карта функций.
} regfn_msg_t;

static int send_full(int sock, const void *buf, size_t len)
{
    const char *p = (const char *)buf;

    while(len > 0) {
        ssize_t w = send(sock, p, len, MSG_NOSIGNAL);
        if(w < 0) {
            if(errno == EINTR) continue;
            return(-1);
        }
        p += w;
        len -= (size_t)w;
    }

    return(0);
}

static int read_full(int sock, void *buf, size_t len)
{
    char *p = (char *)buf;

    while(len > 0) {
        ssize_t r = read(sock, p, len);
        if(r < 0) {
            if(errno == EINTR) continue;
            return(-1);
        }
        if(r == 0) return(-1);
        p += r;
        len -= (size_t)r;
    }

    return(0);
}

static int send_msg_regfn(int sock)
{
    regfn_msg_t msg = {0};

    msg.size = sizeof(msg);
    strncpy(msg.sign, libsrpc_daemon_name, sizeof(msg.sign)-1);
    libsrpc_bmp_func_set(&msg.bmp);

    return send_full(sock, &msg, sizeof(msg));
}

static int read_msg_regfn(int sock, pid_t pid)
{
    regfn_msg_t msg = {0};
    libsrpc_proc_t *proc = libsrpc_proc_get(pid);

    if(!proc) return(-1);

    if(read_full(sock, &msg, sizeof(msg)) < 0) return(-1);
    if(msg.size != sizeof(msg)) return(-1);
    if(strncmp(msg.sign, libsrpc_daemon_name, sizeof(msg.sign)) != 0) return(-1);
    
    libsrpc_reg_func_form_bmp(proc, &msg.bmp);

    return(0);
}

/* ============================================================================== */
static int libsrpc_epoll_create(void)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    return(epfd);
}


static int libsrpc_epoll_add(libsrpc_epoll_t *ed)
{
    int rc = 0;
    struct epoll_event ev;

    if(!ed || ed->epfd <= 0 || ed->socket <= 0 || !ed->func) return(-EINVAL);

    ev.events = ed->events;
    ev.data.ptr = ed;

    rc = epoll_ctl(ed->epfd, EPOLL_CTL_ADD, ed->socket, &ev);

    return(rc);
}


static int libsrpc_unix_scoket_server_create(const char *name)
{
    struct sockaddr_un addr;
    int sck = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(sck < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* abstract socket: first byte = '\0' */
    strncpy(addr.sun_path + 1, name, sizeof(addr.sun_path) - 2);

    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);

/*    if (connect(sck, (struct sockaddr*)&addr, len) < 0) {
        goto err;
    } */
    if (bind(sck, (struct sockaddr*)&addr, len) < 0)
        goto err;

    if (listen(sck, 128) < 0)
        goto err;

    return(sck);
err:
    close(sck);
    return -1;
}


static int send_fd(int sock, int fd_to_send)
{
    struct msghdr msg = {0};

    char dummy = 'X';
    struct iovec io = {
        .iov_base = &dummy,
        .iov_len = sizeof(dummy)
    };

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    *((int *)CMSG_DATA(cmsg)) = fd_to_send;

    msg.msg_controllen = cmsg->cmsg_len;

    return sendmsg(sock, &msg, 0);
}

static int libsrpc_unix_client_worker(libsrpc_server_t *srv, libsrpc_epoll_t *ed, struct epoll_event *ev)
{
    int rc = 0;
    ssize_t rl;
    char buf[32];

    if(!ed || !ev) return(-EINVAL);
    if(ed != ev->data.ptr) return(-EINVAL);

    if (ev->events & (EPOLLHUP | EPOLLRDHUP)) goto close;

    rl = recv(ed->socket, buf, sizeof(buf), MSG_DONTWAIT);
    if(rl == 0) goto close;

end:
    return(rc);
close:
    srv->clients--;
    DBG_PRINT("client closed pid=%d clients=%d\n", ed->pid, srv->clients);
    epoll_ctl(srv->epfd, EPOLL_CTL_DEL, ed->socket, NULL);
    close(ed->socket);
    ed->socket = 0;
    libsrpc_proc_destroy(ed->pid);
    srpc_pool_free(srv->pool_epcln, ed);
    goto end;
}

static int libsrpc_unix_server_addclient(libsrpc_server_t *srv, int sck)
{
    int rc = 0;
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    libsrpc_epoll_t *ed = NULL;
    struct epoll_event ev;

    ed = srpc_pool_alloc(srv->pool_epcln);
    if(!ed) {
        rc = -ENOMEM;
        ERR_PRINT("unix_server: create epoll failed\n");
        goto err;
    }

    rc = getsockopt( sck, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len);
    if(rc) {
        ERR_PRINT("unix_server: getsockopt failed\n");
        goto err;
    }

    ed->epfd = srv->epfd;
    ed->socket = sck;
    ed->events = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
    ed->pid = cred.pid;
    ed->func = libsrpc_unix_client_worker;

    rc = libsrpc_proc_create(ed->pid);
    if(rc < 0) {
        ERR_PRINT("libsrpc_proc_create failed\n");
        goto err;
    }

    /* Сообщение регистрации читается до epoll; таймаут — чтобы демон не блокировался навсегда. */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    rc = setsockopt(sck, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if(rc < 0) {
        ERR_PRINT("setsockopt failed\n");
        goto err;
    }

    rc = read_msg_regfn(sck, ed->pid);
    if(rc < 0) {
        ERR_PRINT("read_reg_msg failed pid=%d\n", ed->pid);
        goto err;
    }
    DBG_PRINT("unix_server: reg msg pid=%d\n", ed->pid);

    if(srv->pshm_fd) {
        rc = send_fd(sck, *srv->pshm_fd);
        if(rc < 0) {
            ERR_PRINT("send_fd failed\n");
            goto err_proc;
        }
    }

    ev.events = ed->events;
    ev.data.ptr = ed;

    rc = epoll_ctl(ed->epfd, EPOLL_CTL_ADD, ed->socket, &ev);
    if(rc < 0) {
        ERR_PRINT("epoll_ctl failed\n");
        goto err_proc;
    }

    srv->clients++;

    DBG_PRINT("client added pid=%d clients=%d\n", ed->pid, srv->clients);
end:
    return(rc);
err_proc:
    libsrpc_proc_destroy(ed->pid);
err:
    if(ed) srpc_pool_free(srv->pool_epcln, ed);
    close(sck);
    goto end;
}


static int libsrpc_unix_server_worker(libsrpc_server_t *srv, libsrpc_epoll_t *ed, struct epoll_event *ev)
{
    int rc = 0;

    if(!ed || !ev) return(-EINVAL);
    if(ed != ev->data.ptr) return(-EINVAL);

    int client_sck = accept4(ed->socket, NULL, NULL, SOCK_CLOEXEC);
    if(client_sck <= 0) {
        rc = -errno;
        ERR_PRINT("unix_server: accept failed\n");
        goto end;
    }

    rc = libsrpc_unix_server_addclient(srv, client_sck);

end:
    return(rc);
}

int libsrpc_unix_server_epoll_wait(libsrpc_server_t *srv, int timeout)
{
    int rc = 0;
    struct epoll_event ev[1024];

    int nev = epoll_wait(srv->epfd, ev, 1024, timeout);
    if(nev < 0) {
        rc = -errno;
        ERR_PRINT("unix_server: epoll_wait failed\n");
        goto err;
    }

    for(int i = 0; i < nev; i++) {
        if(ev[i].data.ptr) ((libsrpc_epoll_t *)ev[i].data.ptr)->func(srv, (libsrpc_epoll_t *)ev[i].data.ptr, &ev[i]);
    }
err:
    return(rc);
}

int libsrpc_unix_server_init(libsrpc_server_t *srv, const char *name)
{
    int rc = 0;

    DBG_PRINT("INIT server\n");

    if(srv->init) goto end;

    srv->pool_epcln = srpc_pool_create(128, sizeof(libsrpc_epoll_t));
    if(!srv->pool_epcln) {
        rc = -ENOMEM;
        ERR_PRINT("create pool failed\n");
        goto err;
    }

    srv->epfd = libsrpc_epoll_create();
    if(srv->epfd < 0) {
        ERR_PRINT("create epoll failed\n");
        rc = -errno;
        goto err;
    }

    srv->socket = libsrpc_unix_scoket_server_create(name);
    if(srv->socket <= 0) {
        rc = -errno;
        if(errno != EADDRINUSE) ERR_PRINT("create socket failed (%d) %s\n", errno, strerror(errno));
        goto err;
    }

    srv->epsrv.epfd = srv->epfd;
    srv->epsrv.socket = srv->socket;
    srv->epsrv.events = EPOLLIN;
    srv->epsrv.func = libsrpc_unix_server_worker;

    rc = libsrpc_epoll_add(&srv->epsrv);
    if(rc < 0) goto err;

    srv->init = 1;
end:
    return(rc);
err:
    if(srv->pool_epcln) srpc_pool_destroy(srv->pool_epcln);
    if(srv->socket > 0) close(srv->socket);
    if(srv->epfd > 0)   close(srv->epfd);
    goto end;
}

int libsrpc_unix_server_exit(libsrpc_server_t *srv)
{
    int rc = 0;
    srpc_pool_fba_t *pool = NULL;
    libsrpc_epoll_t *ed = NULL;
    void *ptr = NULL;

    if(!srv->init) return(0);
    srv->init = 0;

    close(srv->epfd);
    close(srv->socket);

    FBA_FOREACH_POOL(srv->pool_epcln, pool) {
        FBA_FOREACH_BLOCK(pool, ptr) {
            ed = (libsrpc_epoll_t *)ptr;
            if(ed->socket > 0) {
                epoll_ctl(ed->epfd, EPOLL_CTL_DEL, ed->socket, NULL);
                close(ed->socket);
                srpc_pool_free(pool, ptr);
            }
        }
    }
    srpc_pool_destroy(srv->pool_epcln);

    return(rc);
}

/* ============================================================================== */

static int recv_fd(int sock)
{
    struct msghdr msg = {0};
    int fd;

    char dummy;
    struct iovec io = {
        .iov_base = &dummy,
        .iov_len = sizeof(dummy)
    };

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (recvmsg(sock, &msg, 0) < 0) {
        ERR_PRINT("recvmsg failed = %s\n", strerror(errno));
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    if (!cmsg) {
        ERR_PRINT("CMSG_FIRSTHDR failed\n");
        return -1;
    }

    if (cmsg->cmsg_level != SOL_SOCKET) {
        ERR_PRINT("cmsg_level failed\n");
        return -1;
    }

    if (cmsg->cmsg_type != SCM_RIGHTS) {
        ERR_PRINT("cmsg_type failed\n");
        return -1;
    }

    if (cmsg->cmsg_len < CMSG_LEN(sizeof(fd))) {
        return -1;
    }

    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));

    return fd;
}

static int libsrpc_unix_socket_client_create(const char *name)
{
    int rc = 0;
    struct sockaddr_un addr = {0};
    int sck = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(sck < 0) {
        ERR_PRINT("unix_client: socket failed\n");
        return -1;
    }

    addr.sun_family = AF_UNIX;
    /* abstract socket: first byte = '\0' */
    strncpy(addr.sun_path + 1, name, sizeof(addr.sun_path) - 2);

    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);

    for(int i = 0; i < 10000; i++) {
        rc = connect(sck, (struct sockaddr*)&addr, len);
        if(rc < 0) {
            if(errno == ECONNREFUSED) { usleep(100); continue; }
        }
        break;
    }

    if (rc < 0) {
        ERR_PRINT("unix_client: connect failed %s\n", strerror(errno));
        goto err;
    }

    return(sck);
err:
    close(sck);
    return -1;
}

// Управляющий поток для клиента.
static void *unix_client(void *arg)
{
    int rc = 0;
    int epfd = -1;
    struct epoll_event ev = {0};
    libsrpc_client_t *cli = (libsrpc_client_t *)arg;

    cli->socket = libsrpc_unix_socket_client_create(cli->sck_name);
    if(cli->socket <= 0) {
        ERR_PRINT("create socket failed\n");
        goto err_recv_fd;
    }

    rc = send_msg_regfn(cli->socket);
    if(rc < 0) {
        ERR_PRINT("send_msg_regfn failed\n");
        goto err_recv_fd;
    }

    cli->shm_fd = recv_fd(cli->socket);
    if(cli->shm_fd <= 0) {
        ERR_PRINT("recv_fd failed\n");
        goto err_recv_fd;
    }

    rc = libsrpc_shmem_open(&simplerpc_data->shmempool, cli->shm_fd, SHMEM_BASE_VADR, cli->sck_name);
    if(rc < 0) {
        ERR_PRINT("shmem_open failed\n");
        goto err;
    }

    simplerpc_data->proc = libsrpc_proc_get(getpid());
    if(!simplerpc_data->proc) {
        ERR_PRINT("libsrpc_proc_get failed\n");
        goto err;
    }
    simplerpc_data->proc_uid = simplerpc_data->proc->proc_uid;

    rc = libsrpc_thread_rcv_req_start();
    if (rc != 0) {
        ERR_PRINT("libsrpc_thread_rcv_req_start failed\n");
        goto err;
    }

    simplerpc_data->rpc_enable = true;

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if(epfd < 0) {
        ERR_PRINT("epoll_create1 failed\n");
        goto err;
    }

    cli->epfd = epfd;

    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
    ev.data.fd = cli->socket;
    rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ev.data.fd, &ev);
    if(rc < 0) {
        ERR_PRINT("epoll_ctl failed\n");
        goto err;
    }

    /* Будильник для выхода из epoll_wait */
    cli->efd_exit = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(cli->efd_exit < 0) {
        ERR_PRINT("eventfd failed efd_exit\n");
        goto err;
    }
    ev.events = EPOLLIN;
    ev.data.fd = cli->efd_exit;
    rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ev.data.fd, &ev);
    if(rc < 0) {
        ERR_PRINT("epoll_ctl failed efd_exit\n");
        goto err;
    }

    libsrpc_proc_set_run(simplerpc_data->proc); // Устанавливаем статус процесса в режим работы.
    libsrpc_sem_post(&cli->sem_status); // Уведомим что всё инициализировано.

    DBG_PRINT("epoll_wait started\n");
    while(1) {
        char buf[32] = {0};
        memset(&ev, 0, sizeof(ev));
        rc = epoll_wait(epfd, &ev, 1, -1);
        DBG_PRINT("unix_client: epoll_wait %d event = %d\n", rc, ev.events);
        if(rc < 0) {
            ERR_PRINT("epoll_wait failed\n");
            goto err;
        }
        if (ev.data.fd == cli->efd_exit) {
            uint64_t value;
            ssize_t rl = read(ev.data.fd, &value, sizeof(value));
            if(rl < 0) {
                ERR_PRINT("read failed\n");
                goto err;
            }
            if(rl == 0) {
                ERR_PRINT("read failed\n");
                goto err;
            }
            DBG_PRINT("efd_exit %d value = %lu\n", ev.data.fd, value);
            goto end;
        }
        if (ev.events & (EPOLLHUP | EPOLLRDHUP)) goto err;
        if (ev.events & EPOLLIN) {
            rc = recv(ev.data.fd, buf, sizeof(buf), MSG_DONTWAIT);
            if(rc < 0) {
                ERR_PRINT("recv failed\n");
                goto err;
            }
            if(rc == 0) goto err;
        }
    }

end:
    DBG_PRINT("epoll_wait ended\n");
    goto close;
err:
    libsrpc_sem_post(&cli->sem_status); // Уведомим что произошла ошибка.
    ERR_PRINT("error\n");
close:
    simplerpc_data->rpc_enable = false;
    libsrpc_proc_set_stop(simplerpc_data->proc); // Устанавливаем статус процесса в режим остановки.
    libsrpc_thread_rcv_req_stop();
    if(cli->efd_exit > 0) close(cli->efd_exit);
    cli->efd_exit = -1;
    if(epfd > 0) close(epfd);
    if(cli->shm_fd > 0) close(cli->shm_fd);
    cli->shm_fd = -1;
err_recv_fd:
    if(cli->socket > 0) close(cli->socket);
    cli->socket = -1;
    return(NULL);
}


int libsrpc_unix_client_init(libsrpc_client_t *cli, const char *name)
{
    int rc = 0;
    thread_flag_t flags = thread_flag_default();
    flags.name = "unix_client";
    flags.flags |= THREAD_FLAG_DETACHED;

    DBG_PRINT("INIT client: %s\n", name);

    cli->sck_name = name;
    rc = libsrpc_sem_init(&cli->sem_status, 0);
    if(rc < 0) {
        ERR_PRINT("sem_init failed\n");
        goto err;
    }

    rc = pthread_start(unix_client, cli, flags);
    if(rc < 0) {
        ERR_PRINT("unix_client: pthread_start failed\n");
        goto err;
    }

    // Ждём инициализации клиента или ошибки.
    rc = libsrpc_sem_wait(&cli->sem_status);

err:
    return(rc);
}

int libsrpc_unix_client_exit(libsrpc_client_t *cli)
{
    int rc = 0;

    DBG_PRINT("EXIT client: \n");

    uint64_t value = 1;
    write(cli->efd_exit, &value, sizeof(value));

    usleep(1000);

    DBG_PRINT("EXIT client: END\n");
    return(rc);
}


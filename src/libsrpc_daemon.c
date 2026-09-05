
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <dirent.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "libsrpc_local.h"
#include "embedded_loader.h"

#include "libsrpc_proc.h"

#include "libsrpc_daemon.h"

#define ABSTRACT_SOCK_NAME "libsrpc_guard"


void spawn_daemon(const char *daemon_name)
{
    int rc;
    char pathso[PATH_MAX + 32];

    pid_t pid = fork();

    if (pid < 0) return;
    if (pid > 0) {
        waitpid(pid, NULL, 0);
        return;
    }

    /* first child */
    if (setsid() < 0) _exit(1);
    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);

    /* second child */

    /* get current .so path */
    Dl_info info;
    if (!dladdr((void*)spawn_daemon, &info)) {
        _exit(1);
    }
    snprintf(pathso, sizeof(pathso), "LIBSIMPLERPC_SO=%s",info.dli_fname);
    DBG_PRINT("!!! SO = %s\n",info.dli_fname);

    /* create anonymous executable file */
    int fd = memfd_create("shmguard_loader", MFD_CLOEXEC);
    if (fd < 0) _exit(1);
    /* write embedded ELF */
    if (write(fd, loader, loader_len) != loader_len) {
        _exit(1);
    }
    lseek(fd, 0, SEEK_SET);
    /* executable permission */
    rc = fchmod(fd, 0700);
    if(rc < 0) {
        WRN_PRINT("fchmod failed: %s\n", strerror(errno));
    }

    /* execute directly from RAM */
    //char* const argv[] = { (char*)"[shmguardd]", NULL };
    char* const argv[] = { (char*)daemon_name, NULL };
    char* const env[] = { (char*)pathso, NULL };
    fexecve(fd, argv, env);

    _exit(1);
}


int simplerpc_daemon_main(int ac __attribute__((unused)), char *av[])
{
    int rc;
    char *daemon_name = av[0];

    DBG_PRINT("START daemon = '%s'\n",daemon_name);

    simplerpc_data->is_daemon = true;
    simplerpc_data->proc_uid = DAEMON_PROC_UID;

DBG_PRINT("INIT server\n");
    /* Инициализируем сервер Unix сокетов. */
    rc = libsrpc_unix_server_init(&simplerpc_data->srv, daemon_name);
    if(rc < 0) goto err_init;
DBG_PRINT("INIT shmem\n");
    /* Инициализируем разделяемую память. */
    rc = libsrpc_shmem_create(&simplerpc_data->shmempool, daemon_name, SHMEM_SIZE, SHMEM_BASE_VADR);
    if(rc < 0) goto err_init;
DBG_PRINT("INIT pshm_fd\n");
    simplerpc_data->srv.pshm_fd = &simplerpc_data->shmempool.shm_fd;

    /* Ждём клиентов. */
    DBG_PRINT("WAIT for clients\n");
    while(1) {
        rc = libsrpc_unix_server_epoll_wait(&simplerpc_data->srv, 1000);
        if(rc < 0) break;
        if(simplerpc_data->srv.clients == 0) break;
    }

    /* Завершаем сервер Unix сокетов. */
    libsrpc_unix_server_exit(&simplerpc_data->srv);

end:
    /* Уничтожаем разделяемую память. */
    if(simplerpc_data->shmempool.shm_fd > 0) libsrpc_shmem_destroy(&simplerpc_data->shmempool);
    DBG_PRINT("EXIT\n");
    return rc;
err_init:
    DBG_PRINT("Error: %s\n", strerror(-rc));
    goto end;
}


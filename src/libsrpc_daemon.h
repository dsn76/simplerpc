#ifndef FILE_LIBSRPC_DAEMON_H
#define FILE_LIBSRPC_DAEMON_H

void spawn_daemon(const char *daemon_name);
int simplerpc_daemon_main(int ac __attribute__((unused)), char *av[]);

#endif // FILE_LIBSRPC_DAEMON_H

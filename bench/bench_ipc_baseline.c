/* Базовые линии межпроцессного round-trip для сравнения с sRPC.
 * Библиотека libsrpc здесь не используется намеренно.
 *
 * Полезная нагрузка совпадает с профилем calc_add(int,int) -> int:
 * запрос 8 байт, ответ 4 байта.
 *
 * Использование: bench_ipc_baseline [итераций]
 */

#include "bench_common.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { int a, b; } req_t;

/* Эхо-сервер: читает запрос, возвращает сумму. */
static void echo_child(int fd_req, int fd_rsp)
{
    req_t q;
    while (read(fd_req, &q, sizeof(q)) == (ssize_t)sizeof(q)) {
        int r = q.a + q.b;
        if (write(fd_rsp, &r, sizeof(r)) != (ssize_t)sizeof(r)) break;
    }
    _exit(0);
}

static int measure(const char *name, int fd_req, int fd_rsp,
                   size_t iters, uint64_t *s)
{
    req_t q = { 1, 2 };
    int r = 0;

    for (int i = 0; i < 100; i++) {
        if (write(fd_req, &q, sizeof(q)) != (ssize_t)sizeof(q)) return -1;
        if (read(fd_rsp, &r, sizeof(r)) != (ssize_t)sizeof(r)) return -1;
    }

    uint64_t wall0 = bench_now_ns();
    for (size_t i = 0; i < iters; i++) {
        uint64_t t0 = bench_now_ns();
        if (write(fd_req, &q, sizeof(q)) != (ssize_t)sizeof(q)) return -1;
        if (read(fd_rsp, &r, sizeof(r)) != (ssize_t)sizeof(r)) return -1;
        uint64_t t1 = bench_now_ns();
        if (r != 3) return -1;
        s[i] = t1 - t0;
    }
    uint64_t wall1 = bench_now_ns();

    bench_report(name, s, iters, wall1 - wall0);
    return 0;
}

int main(int ac, char **av)
{
    size_t iters = (ac > 1) ? (size_t)strtoul(av[1], NULL, 10) : 4000;
    if (iters == 0 || iters > 10000000) iters = 4000;
    uint64_t *s = calloc(iters, sizeof(*s));
    if (!s) return 1;

    printf("bench_ipc_baseline: pid=%d итераций=%zu\n", getpid(), iters);

    /* --- socketpair (AF_UNIX, SOCK_STREAM) --- */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return 1;

        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            echo_child(sv[1], sv[1]);
        }
        close(sv[1]);
        measure("unix socketpair RTT", sv[0], sv[0], iters, s);
        close(sv[0]);
        waitpid(pid, NULL, 0);
    }

    /* --- пара каналов pipe(2) --- */
    {
        int to_child[2], to_parent[2];
        if (pipe(to_child) < 0 || pipe(to_parent) < 0) return 1;

        pid_t pid = fork();
        if (pid == 0) {
            close(to_child[1]);
            close(to_parent[0]);
            echo_child(to_child[0], to_parent[1]);
        }
        close(to_child[0]);
        close(to_parent[1]);
        measure("pipe RTT", to_child[1], to_parent[0], iters, s);
        close(to_child[1]);
        close(to_parent[0]);
        waitpid(pid, NULL, 0);
    }

    return 0;
}

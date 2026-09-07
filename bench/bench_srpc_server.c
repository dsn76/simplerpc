/* Экспортёр RPC-функции для замеров: определяет calc_add() локально,
 * поэтому сильный символ перекрывает weak alias из libsrpc.so
 * и процесс регистрируется как исполнитель calc_add.
 *
 * Живёт до SIGTERM/SIGINT либо до истечения --seconds.
 */

#define _GNU_SOURCE

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libsrpc.h"

static volatile sig_atomic_t stop_flag = 0;
static atomic_long calls = 0;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

int calc_add(int a, int b)
{
    atomic_fetch_add(&calls, 1);
    return a + b;
}

int main(int ac, char **av)
{
    int seconds = 60;

    if (ac > 1) seconds = atoi(av[1]);
    if (seconds <= 0 || seconds > 3600) seconds = 60;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    /* Обращение к символу из libsrpc.so обязательно: иначе линкер с
     * --as-needed выбросит DT_NEEDED и процесс не подключится к пулу.
     */
    void *probe = libsrpc_shmem_malloc(64);
    printf("bench_srpc_server: pid=%d pool_probe=%p\n", getpid(), probe);
    fflush(stdout);
    libsrpc_shmem_free(probe);

    for (int i = 0; i < seconds * 10 && !stop_flag; i++) usleep(100000);

    printf("bench_srpc_server: pid=%d обработано вызовов=%ld\n",
           getpid(), atomic_load(&calls));
    return 0;
}

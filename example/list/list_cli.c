#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <getopt.h>

#include "libsrpc.h"

#include "dal_list.h"

typedef struct list_cli_node_s {
    dal_list_node_t node;
    size_t size;
    uint8_t data[0];
} list_cli_node_t;


static int list_cli_add(const char *data)
{
    size_t len = strlen(data);
    if(len == 0) {
        return -EINVAL;
    }
    list_cli_node_t *node = (list_cli_node_t *)libsrpc_shmem_malloc(sizeof(*node) + len + 1);
    if(!node) {
        printf("ERROR: libsrpc_shmem_malloc() failed\n");
        return -1;
    }
    node->size = sizeof(*node) + len + 1;
    memcpy(node->data, data, len);
    node->data[len] = '\0';
    void *ptr = list_api(DAL_LIST_OP_INSERT, node);
    if(!ptr) {
        printf("ERROR: list_api(insert) failed\n");
        libsrpc_shmem_free(node);
        return -ENOMEM;
    }
    libsrpc_shmem_free(node);
    return 0;
}

static int list_cli_delete(const char *data)
{
    int rc = 0;
    list_cli_node_t *node = NULL;

    size_t len = strlen(data);
    if(len == 0) {
        return -EINVAL;
    }

    dal_list_head_t *list_head = (dal_list_head_t *)list_api(DAL_LIST_OP_GET_HEAD, NULL);
    if(!list_head) {
        printf("ERROR: list_api(get_head) failed\n");
        return -1;
    }

    int slot = libsrpc_shmem_proc_lock(0, list_head);
    if(slot < 0) {
        printf("ERROR: libsrpc_shmem_proc_lock() failed\n");
        return -1;
    }

    dal_list_node_t *itnode = NULL;
    dal_list_node_t *tmp = NULL;
    rc = dal_list_lock(list_head);
    if(rc != 0) {
        printf("ERROR: dal_list_lock() failed\n");
        return -1;
    }
    DAL_LIST_FOREACH_SAFE(list_head, itnode, tmp) {
        list_cli_node_t *cli_node = (list_cli_node_t *)itnode;
        if(strcmp((const char *)cli_node->data, data) == 0) {
            node = cli_node;
            break;
        }
    }
    rc = dal_list_unlock(list_head);
    if(rc != 0) {
        printf("ERROR: dal_list_unlock() failed\n");
        return -1;
    }

    rc = libsrpc_shmem_proc_unlock(slot);
    if(rc != 0) {
        printf("ERROR: libsrpc_shmem_proc_unlock() failed\n");
        return -1;
    }

    if(!node) {
        printf("ERROR: node not found\n");
        return -ENOENT;
    }

    node = list_api(DAL_LIST_OP_REMOVE, node);
    if(!node) {
        printf("ERROR: list_api(delete) failed\n");
        return -1;
    }

    return 0;
}

static int list_cli_print(void)
{
    int rc = 0;

    dal_list_head_t *list_head = (dal_list_head_t *)list_api(DAL_LIST_OP_GET_HEAD, NULL);
    if(!list_head) {
        printf("ERROR: list_api(get_head) failed\n");
        return -1;
    }

    int slot = libsrpc_shmem_proc_lock(0, list_head);
    if(slot < 0) {
        printf("ERROR: libsrpc_shmem_proc_lock() failed\n");
        return -1;
    }

    dal_list_node_t *itnode = NULL;
    dal_list_node_t *tmp = NULL;
    rc = dal_list_lock(list_head);
    if(rc != 0) {
        printf("ERROR: dal_list_lock() failed\n");
        return -1;
    }
    DAL_LIST_FOREACH_SAFE(list_head, itnode, tmp) {
        list_cli_node_t *cli_node = (list_cli_node_t *)itnode;
        printf("size: %zu, data: %s\n", cli_node->size, cli_node->data);
    }
    rc = dal_list_unlock(list_head);
    if(rc != 0) {
        printf("ERROR: dal_list_unlock() failed\n");
        return -1;
    }

    rc = libsrpc_shmem_proc_unlock(slot);
    if(rc != 0) {
        printf("ERROR: libsrpc_shmem_proc_unlock() failed\n");
        return -1;
    }

    return 0;
}

static int list_cli_lock(const char *time)
{
    int rc = 0;

    int lock_time = atoi(time);
    if(lock_time <= 0) {
        fprintf(stderr, "ERROR: invalid lock time: %s\n", time);
        return -EINVAL;
    }

    dal_list_head_t *list_head = (dal_list_head_t *)list_api(DAL_LIST_OP_GET_HEAD, NULL);
    if(!list_head) {
        printf("ERROR: list_api(get_head) failed\n");
        return -1;
    }

    int slot = libsrpc_shmem_proc_lock(0, list_head);
    if(slot < 0) {
        printf("ERROR: libsrpc_shmem_proc_lock() failed\n");
        return -1;
    }

    sleep(lock_time);

    rc = libsrpc_shmem_proc_unlock(slot);
    if(rc != 0) {
        printf("ERROR: libsrpc_shmem_proc_unlock() failed\n");
        return -1;
    }
    return 0;
}

static void list_cli_usage(FILE *out, const char *prog)
{
    fprintf(out,
        "Usage: %s [OPTION]...\n"
        "\n"
        "Options:\n"
        "  --add STRING    add STRING to the list\n"
        "  --del STRING    delete STRING from the list\n"
        "  --print         print the list\n"
        "  -h, --help      display this help and exit\n"
        "\n"
        "Multiple --add, --del and --print options may be given;\n"
        "they are executed in the order they appear.\n",
        prog);
}

int main(int ac, char **av)
{
    int rc = 0;
    int opt = 0;
    int acted = 0;

    static struct option long_options[] = {
        {"add",  required_argument, NULL, 'a'},
        {"del",  required_argument, NULL, 'd'},
        {"print", no_argument,      NULL, 'p'},
        {"lock", required_argument, NULL, 'l'},
        {"help", no_argument,       NULL, 'h'},
        {NULL,   0,                 NULL, 0},
    };

    while((opt = getopt_long(ac, av, "h", long_options, NULL)) != -1) {
        switch(opt) {
            case 'a':
                rc = list_cli_add(optarg);
                if(rc != 0) {
                    fprintf(stderr, "ERROR: list_cli_add() failed\n");
                    return 1;
                }
                acted = 1;
                break;

            case 'd':
                rc = list_cli_delete(optarg);
                if(rc != 0) {
                    fprintf(stderr, "ERROR: list_cli_delete() failed\n");
                    return 1;
                }
                acted = 1;
                break;

            case 'p':
                rc = list_cli_print();
                if(rc != 0) {
                    fprintf(stderr, "ERROR: list_cli_print() failed\n");
                    return 1;
                }
                acted = 1;
                break;

            case 'l':
                rc = list_cli_lock(optarg);
                if(rc != 0) {
                    fprintf(stderr, "ERROR: list_cli_lock() failed\n");
                    return 1;
                }
                acted = 1;
                break;

            case 'h':
                list_cli_usage(stdout, av[0]);
                return 0;

            default:
                list_cli_usage(stderr, av[0]);
                return 1;
        }
    }

    if(optind < ac) {
        fprintf(stderr, "ERROR: unexpected argument: %s\n", av[optind]);
        list_cli_usage(stderr, av[0]);
        return 1;
    }

    if(!acted) {
        list_cli_usage(stderr, av[0]);
        return 1;
    }

    return 0;
}


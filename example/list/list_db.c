#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

#include "libsrpc.h"

#include "dal_list.h"

static dal_list_head_t *list_head = NULL;

void * list_api(int op, void *node)
{
    int rc = 0;
    dal_list_node_t *itnode = NULL;
    dal_list_node_t *tmp = NULL;

    printf("list_api: op: %d, node: %p\n", op, node);
    switch(op) {
        case DAL_LIST_OP_GET_HEAD:
            return list_head;

        case DAL_LIST_OP_INSERT:
            rc = libsrpc_shmem_link(node);
            if(rc != 0) {
                return NULL;
            }
            dal_list_insert(list_head, (dal_list_node_t *)node);
            return node;

        case DAL_LIST_OP_REMOVE:
            dal_list_lock(list_head);
            DAL_LIST_FOREACH_SAFE(list_head, itnode, tmp) {
                if(itnode == (dal_list_node_t *)node) {
                    dal_list_remove(list_head, (dal_list_node_t *)node);
                    libsrpc_shmem_free(node);
                    dal_list_unlock(list_head);
                    return node;
                }
            }
            dal_list_unlock(list_head);
            return NULL;
    } // switch(op)
    return NULL;
}

atomic_int_least32_t exit_flag = 0;

void all_exit(void) {
    atomic_store_explicit(&exit_flag, 1, memory_order_release);
}

int main(int ac, char **av)
{
    printf("%s START\n", av[0]);

    int fnnum = libsrpc_fnreg_num_get(libsrpc_funid_list_api);
    if(fnnum > 1) {
        printf("ERROR: list_api function is already registered. Exit.\n");
        return -1;
    }

    list_head = (dal_list_head_t *)libsrpc_shmem_malloc(sizeof(*list_head));
    if(!list_head) {
        printf("ERROR: libsrpc_shmem_malloc() failed\n");
        return -1;
    }
    int rc = dal_list_init(list_head);
    if(rc != 0) {
        printf("ERROR: dal_list_init() failed\n");
        return -1;
    }
    printf("list_head: %p\n", (void*)list_head);

    while (!atomic_load_explicit(&exit_flag, memory_order_acquire)) {
        usleep(1000);
    }

    printf("%s END\n", av[0]);

    return 0;
}


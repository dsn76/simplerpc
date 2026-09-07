#ifndef LIBSRPC_LIST_SPIN_H
#define LIBSRPC_LIST_SPIN_H

#include <stdatomic.h>
#include <stddef.h>
#include <pthread.h>

typedef struct libsrpc_list_node {
    _Atomic(struct libsrpc_list_node *) next;
    _Atomic(struct libsrpc_list_head *) head;
} libsrpc_list_node_t;

typedef struct libsrpc_list_head {
    pthread_spinlock_t         lock;
    _Atomic(libsrpc_list_node_t *)     first;
    atomic_ulong               write_ops;
} libsrpc_list_head_t;

typedef enum {
    LIBSRPC_LIST_OK        =  0,
    LIBSRPC_LIST_EINVAL    = -1,
    LIBSRPC_LIST_ENOTFOUND = -2,
    LIBSRPC_LIST_EALREADY  = -3,
    LIBSRPC_LIST_ELOCK     = -4,
} libsrpc_list_err_t;

int  libsrpc_list_head_init(libsrpc_list_head_t *head);
int  libsrpc_list_head_destroy(libsrpc_list_head_t *head);
void libsrpc_list_node_init(libsrpc_list_node_t *node);
int  libsrpc_list_push_front(libsrpc_list_head_t *head, libsrpc_list_node_t *node);
int  libsrpc_list_remove(libsrpc_list_node_t *node);

#define LIBSRPC_LIST_ENTRY(node_ptr, type, member) \
    ((type *)((char *)(node_ptr) - offsetof(type, member)))

#define LIBSRPC_LIST_FOREACH(head_ptr, entry_ptr, type, member)                      \
    for (libsrpc_list_node_t *__lf_n =                                               \
             atomic_load_explicit(&(head_ptr)->first, memory_order_acquire); \
         (__lf_n != NULL) &&                                                 \
             ((entry_ptr) = LIBSRPC_LIST_ENTRY(__lf_n, type, member), 1);            \
         __lf_n = atomic_load_explicit(&__lf_n->next, memory_order_acquire))

#endif /* LIBSRPC_LIST_SPIN_H */

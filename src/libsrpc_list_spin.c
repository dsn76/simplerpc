#include "libsrpc_list_spin.h"
#include <errno.h>

int libsrpc_list_head_init(libsrpc_list_head_t *head)
{
    if (head == NULL)
        return LIBSRPC_LIST_EINVAL;

    int rc = pthread_spin_init(&head->lock, PTHREAD_PROCESS_PRIVATE);
    if (rc != 0)
        return LIBSRPC_LIST_ELOCK;

    atomic_store_explicit(&head->first, NULL, memory_order_relaxed);
    atomic_store_explicit(&head->write_ops, 0, memory_order_relaxed);
    return LIBSRPC_LIST_OK;
}

int libsrpc_list_head_destroy(libsrpc_list_head_t *head)
{
    if (head == NULL)
        return LIBSRPC_LIST_EINVAL;

    int rc = pthread_spin_destroy(&head->lock);
    if (rc != 0)
        return LIBSRPC_LIST_ELOCK;

    return LIBSRPC_LIST_OK;
}

void libsrpc_list_node_init(libsrpc_list_node_t *node)
{
    if (node == NULL)
        return;

    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    node->head = NULL;
}

int libsrpc_list_push_front(libsrpc_list_head_t *head, libsrpc_list_node_t *node)
{
    if (head == NULL || node == NULL)
        return LIBSRPC_LIST_EINVAL;

    pthread_spin_lock(&head->lock);

    if (node->head != NULL) {
        pthread_spin_unlock(&head->lock);
        return LIBSRPC_LIST_EALREADY;
    }

    libsrpc_list_node_t *old = atomic_load_explicit(&head->first, memory_order_relaxed);
    atomic_store_explicit(&node->next, old, memory_order_relaxed);
    node->head = head;
    atomic_store_explicit(&head->first, node, memory_order_release);
    atomic_fetch_add_explicit(&head->write_ops, 1, memory_order_relaxed);

    pthread_spin_unlock(&head->lock);
    return LIBSRPC_LIST_OK;
}

int libsrpc_list_remove(libsrpc_list_node_t *node)
{
    if (node == NULL)
        return LIBSRPC_LIST_EINVAL;

    libsrpc_list_head_t *head = node->head;
    if (head == NULL)
        return LIBSRPC_LIST_ENOTFOUND;

    pthread_spin_lock(&head->lock);

    libsrpc_list_node_t *cur = atomic_load_explicit(&head->first, memory_order_relaxed);
    libsrpc_list_node_t *prev = NULL;
    int found = 0;

    while (cur != NULL) {
        if (cur == node) {
            found = 1;
            break;
        }
        prev = cur;
        cur = atomic_load_explicit(&cur->next, memory_order_relaxed);
    }

    if (!found) {
        pthread_spin_unlock(&head->lock);
        return LIBSRPC_LIST_ENOTFOUND;
    }

    libsrpc_list_node_t *next_of_node = atomic_load_explicit(&node->next, memory_order_relaxed);

    if (prev == NULL) {
        atomic_store_explicit(&head->first, next_of_node, memory_order_release);
    } else {
        atomic_store_explicit(&prev->next, next_of_node, memory_order_release);
    }

    atomic_fetch_add_explicit(&head->write_ops, 1, memory_order_relaxed);

    node->head = NULL;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

    pthread_spin_unlock(&head->lock);
    return LIBSRPC_LIST_OK;
}

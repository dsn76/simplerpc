#ifndef FILE_DAL_LIST_H
#define FILE_DAL_LIST_H

/* DAL = Data Abstraction Layer */
/* List = Linked List */

#include <pthread.h>
#include <stdint.h>
#include <errno.h>

typedef enum dal_list_op_e {
    DAL_LIST_OP_GET_HEAD = 1,
    DAL_LIST_OP_INSERT = 2,
    DAL_LIST_OP_REMOVE = 3,
} dal_list_op_t;

typedef struct dal_list_node_s dal_list_node_t;
typedef struct dal_list_node_s {
    dal_list_node_t *prev;
    dal_list_node_t *next;
} dal_list_node_t;

typedef struct dal_list_head_s {
    dal_list_node_t sentinel;
    uint32_t magic;
    uint32_t count;
    pthread_mutex_t mutex;
} dal_list_head_t;

#define DAL_LIST_MAGIC 0x56789012

#define DAL_LIST_FOREACH_SAFE(head, node, tmp) \
    for(node = (head)->sentinel.next, tmp = node->next; node != &head->sentinel; node = tmp, tmp = node->next)

static inline void dal_list_node_init(dal_list_node_t *node) {
    node->prev = NULL;
    node->next = NULL;
}

static inline int dal_list_init(dal_list_head_t *head) {
    pthread_mutexattr_t attr;
    int rc = pthread_mutexattr_init(&attr);
    if(rc != 0) {
        return rc;
    }
    rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if(rc != 0) {
        pthread_mutexattr_destroy(&attr);
        return rc;
    }
    rc =pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    if(rc != 0) {
        pthread_mutexattr_destroy(&attr);
        return rc;
    }
    rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if(rc != 0) {
        pthread_mutexattr_destroy(&attr);
        return rc;
    }
    rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if(rc != 0) {
        pthread_mutexattr_destroy(&attr);
        return rc;
    }
    rc = pthread_mutex_init(&head->mutex, &attr);
    if(rc != 0) {
        pthread_mutexattr_destroy(&attr);
        return rc;
    }
    pthread_mutexattr_destroy(&attr);

    head->magic = DAL_LIST_MAGIC;
    head->count = 0;
    head->sentinel.prev = &head->sentinel;
    head->sentinel.next = &head->sentinel;
    return 0;
}

static inline void dal_list_destroy(dal_list_head_t *head) {
    pthread_mutex_destroy(&head->mutex);
    head->magic = 0;
    head->count = 0;
}

static inline int dal_list_lock(dal_list_head_t *head) {
    int rc = pthread_mutex_lock(&head->mutex);
    if(rc == EOWNERDEAD) {
        rc = pthread_mutex_consistent(&head->mutex);
    }
    return rc;
}

static inline int dal_list_unlock(dal_list_head_t *head) {
    return pthread_mutex_unlock(&head->mutex);
}

static inline int dal_list_insert(dal_list_head_t *head, dal_list_node_t *node) {
    dal_list_lock(head);
    node->prev = &head->sentinel;
    node->next = head->sentinel.next;
    head->sentinel.next->prev = node;
    head->sentinel.next = node;
    head->count++;
    dal_list_unlock(head);
    return 0;
}

static inline int dal_list_remove(dal_list_head_t *head, dal_list_node_t *node) {
    dal_list_lock(head);
    if(node->prev == NULL || node->next == NULL) {
        pthread_mutex_unlock(&head->mutex);
        return -1;
    }
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
    head->count--;
    dal_list_unlock(head);
    return 0;
}

#endif // FILE_DAL_LIST_H

#define _GNU_SOURCE

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "libsrpc_list.h"


/*
 * Инициализация списка.
 */
int
list_init(list_head_t *list, unsigned int barrier_threads)
{
    int ret;

    if (list == NULL)
        return EINVAL;

    list->first = NULL;
    list->last  = NULL;
    list->count = 0;
    list->barrier_initialized = 0;

    // Robust mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    ret = pthread_mutex_init(&list->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
//    ret = pthread_mutex_init(&list->mutex, NULL);
    if (ret != 0)
        return ret;

    ret = pthread_cond_init(&list->cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&list->mutex);
        return ret;
    }

    if (barrier_threads > 0) {
        ret = pthread_barrier_init(
            &list->barrier,
            NULL,
            barrier_threads
        );

        if (ret != 0) {
            pthread_cond_destroy(&list->cond);
            pthread_mutex_destroy(&list->mutex);
            return ret;
        }

        list->barrier_initialized = 1;
    }

    return 0;
}


/*
 * Уничтожение списка.
 *
 * ВАЖНО:
 * список должен быть пустым, а все рабочие потоки
 * должны уже закончить работу с ним.
 */
int
list_destroy(list_head_t *list)
{
    int ret;

    if (list == NULL)
        return EINVAL;

    pthread_mutex_lock(&list->mutex);

    if (list->count != 0) {
        pthread_mutex_unlock(&list->mutex);
        return EBUSY;
    }

    pthread_mutex_unlock(&list->mutex);

    if (list->barrier_initialized) {
        ret = pthread_barrier_destroy(&list->barrier);
        if (ret != 0)
            return ret;
    }

    pthread_cond_destroy(&list->cond);
    pthread_mutex_destroy(&list->mutex);

    return 0;
}

int list_lock(list_head_t *list)
{
    int ret = pthread_mutex_lock(&list->mutex);
    if (ret == EOWNERDEAD) {
        pthread_mutex_consistent(&list->mutex);
    }
    return ret;
}

int list_unlock(list_head_t *list)
{
    return pthread_mutex_unlock(&list->mutex);
}

/*
 * Добавить элемент в голову списка.
 *
 * Предполагается, что node не находится
 * одновременно в другом списке.
 */
int
list_add_head(list_head_t *list, list_node_t *node)
{
    if (list == NULL || node == NULL)
        return EINVAL;

    pthread_mutex_lock(&list->mutex);

    node->prev = NULL;
    node->next = list->first;

    if (list->first != NULL)
        list->first->prev = node;
    else
        list->last = node;

    list->first = node;
    list->count++;

    /*
     * Разбудить поток, который ждёт появления элемента.
     */
    pthread_cond_signal(&list->cond);

    pthread_mutex_unlock(&list->mutex);

    return 0;
}


/*
 * Добавить элемент в хвост списка.
 */
int
list_add_tail(list_head_t *list, list_node_t *node)
{
    if (list == NULL || node == NULL)
        return EINVAL;

    pthread_mutex_lock(&list->mutex);

    node->next = NULL;
    node->prev = list->last;

    if (list->last != NULL)
        list->last->next = node;
    else
        list->first = node;

    list->last = node;
    list->count++;

    pthread_cond_signal(&list->cond);

    pthread_mutex_unlock(&list->mutex);

    return 0;
}


/*
 * Удалить элемент.
 *
 * node должен принадлежать этому списку.
 */
int
list_remove(list_head_t *list, list_node_t *node)
{
    if (list == NULL || node == NULL)
        return EINVAL;

    pthread_mutex_lock(&list->mutex);

    if (node->prev != NULL)
        node->prev->next = node->next;
    else
        list->first = node->next;

    if (node->next != NULL)
        node->next->prev = node->prev;
    else
        list->last = node->prev;

    node->prev = NULL;
    node->next = NULL;

    if (list->count > 0)
        list->count--;

    pthread_mutex_unlock(&list->mutex);

    return 0;
}


/*
 * Извлечь первый элемент.
 *
 * Возвращает NULL, если список пуст.
 */
list_node_t *
list_pop_head(list_head_t *list)
{
    list_node_t *node;

    if (list == NULL)
        return NULL;

    pthread_mutex_lock(&list->mutex);

    node = list->first;

    if (node != NULL) {
        list->first = node->next;

        if (list->first != NULL)
            list->first->prev = NULL;
        else
            list->last = NULL;

        node->prev = NULL;
        node->next = NULL;

        list->count--;
    }

    pthread_mutex_unlock(&list->mutex);

    return node;
}


/*
 * Получить количество элементов.
 */
size_t
list_count(list_head_t *list)
{
    size_t count;

    if (list == NULL)
        return 0;

    pthread_mutex_lock(&list->mutex);
    count = list->count;
    pthread_mutex_unlock(&list->mutex);

    return count;
}


/*
 * Ждать, пока список не станет непустым.
 *
 * Это пример использования condition variable.
 */
void
list_wait_nonempty(list_head_t *list)
{
    if (list == NULL)
        return;

    pthread_mutex_lock(&list->mutex);

    while (list->count == 0)
        pthread_cond_wait(&list->cond, &list->mutex);

    pthread_mutex_unlock(&list->mutex);
}


/*
 * Барьер списка.
 *
 * Все потоки должны вызвать эту функцию.
 */
int
list_barrier_wait(list_head_t *list)
{
    if (list == NULL || !list->barrier_initialized)
        return EINVAL;

    return pthread_barrier_wait(&list->barrier);
}


#if 0
/*
 * ============================================================
 *                    ПРИМЕР ИСПОЛЬЗОВАНИЯ
 * ============================================================
 */

typedef struct my_item {
    int value;
    char name[32];

    /*
     * Именно этот элемент пользователь добавляет
     * в свою структуру.
     */
    list_node_t link;

} my_item_t;


static void *
worker(void *arg)
{
    list_head_t *list = arg;

    /*
     * Все потоки стартуют одновременно.
     */
    list_barrier_wait(list);

    /*
     * Ждём появления элемента.
     */
    list_wait_nonempty(list);

    /*
     * Обход необходимо делать под mutex,
     * если другие потоки могут менять список.
     */
    pthread_mutex_lock(&list->mutex);

    list_node_t *node;

    LIST_FOREACH(list, node) {
        my_item_t *item =
            LIST_ENTRY(node, my_item_t, link);

        printf("thread: value=%d name=%s\n",
               item->value,
               item->name);
    }

    pthread_mutex_unlock(&list->mutex);

    return NULL;
}


int main1(void)
{
    list_head_t list;

    /*
     * Допустим, четыре рабочих потока.
     */
    if (list_init(&list, 4) != 0)
        return EXIT_FAILURE;

    pthread_t threads[4];

    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &list);


    /*
     * Добавляем элементы.
     */
    for (int i = 0; i < 10; ++i) {
        my_item_t *item = malloc(sizeof(*item));

        if (item == NULL)
            break;

        item->value = i;
        snprintf(item->name,
                 sizeof(item->name),
                 "item-%d",
                 i);

        list_add_tail(&list, &item->link);
    }


    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);


    /*
     * Удаляем и освобождаем элементы.
     */
    for (;;) {
        list_node_t *node = list_pop_head(&list);

        if (node == NULL)
            break;

        my_item_t *item =
            LIST_ENTRY(node, my_item_t, link);

        free(item);
    }


    list_destroy(&list);

    return EXIT_SUCCESS;
}
#endif
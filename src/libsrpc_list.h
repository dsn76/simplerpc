#ifndef FILE_LIBSRPC_LIST_H
#define FILE_LIBSRPC_LIST_H

/*
 * Два основных typedef:
 *
 * list_node_t — элемент, который пользователь встраивает
 *               в свою структуру.
 *
 * list_head_t — голова списка и служебная информация.
 */

 typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} list_node_t;

typedef struct list_head {
    list_node_t *first;
    list_node_t *last;

    size_t count;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    /*
     * Количество потоков, которые должны синхронизироваться
     * перед началом работы.
     *
     * Если барьер не нужен — barrier_initialized = 0.
     */
    pthread_barrier_t barrier;
    int barrier_initialized;
} list_head_t;


/*
 * Получить адрес структуры, содержащей list_node_t.
 *
 * Пример:
 *
 * struct my_item {
 *     int value;
 *     list_node_t link;
 * };
 *
 * struct my_item *item = LIST_ENTRY(node, struct my_item, link);
 */
#define LIST_ENTRY(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))


/*
 * ============================================================
 *              МАКРОСЫ ДЛЯ ПЕРЕБОРА
 * ============================================================
 *
 * ВАЖНО:
 * эти макросы не удерживают mutex.
 *
 * Поэтому они безопасны только в том случае, если:
 *
 *   1. другие потоки не изменяют список во время обхода;
 *
 * или
 *
 *   2. вызывающий код сам удерживает list->mutex.
 */


/*
 * Перебор от головы к хвосту.
 *
 * node — переменная типа list_node_t *
 */
 #define LIST_FOREACH(list, node)                                      \
 for ((node) = (list)->first;                                     \
      (node) != NULL;                                             \
      (node) = (node)->next)


/*
* Перебор от хвоста к голове.
*/
#define LIST_FOREACH_REVERSE(list, node)                             \
 for ((node) = (list)->last;                                      \
      (node) != NULL;                                             \
      (node) = (node)->prev)


/*
* Перебор с возможностью удаления текущего элемента.
*
* next_node заранее сохраняет следующий элемент.
*/
#define LIST_FOREACH_SAFE(list, node, next_node)                     \
 for ((node) = (list)->first;                                     \
      (node) != NULL &&                                          \
      ((next_node) = (node)->next, 1);                           \
      (node) = (next_node))


/*
* Более удобный вариант:
* сразу получить пользовательскую структуру.
*
* Например:
*
* LIST_FOREACH_ENTRY(list, item, link, struct my_item) {
*     printf("%d\n", item->value);
* }
*/
#define LIST_FOREACH_ENTRY(list, pos, member, type)                  \
 for (list_node_t *__node = (list)->first;                       \
      __node != NULL &&                                          \
      ((pos) = LIST_ENTRY(__node, type, member), 1);             \
      __node = __node->next)


/*
* Безопасный вариант для удаления элементов.
*/
#define LIST_FOREACH_ENTRY_SAFE(list, pos, tmp, member, type)        \
 for (list_node_t *__node = (list)->first, *__next;              \
      __node != NULL &&                                          \
      ((__next = __node->next),                                   \
       (pos) = LIST_ENTRY(__node, type, member), 1);             \
      __node = __next)



int list_init(list_head_t *list, unsigned int barrier_threads);
int list_destroy(list_head_t *list);
int list_lock(list_head_t *list);
int list_unlock(list_head_t *list);
int list_add_head(list_head_t *list, list_node_t *node);
int list_add_tail(list_head_t *list, list_node_t *node);
int list_remove(list_head_t *list, list_node_t *node);
size_t list_count(list_head_t *list);
void list_wait_nonempty(list_head_t *list);
int list_barrier_wait(list_head_t *list);


#endif // FILE_LIBSRPC_LIST_H

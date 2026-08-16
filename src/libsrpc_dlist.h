#ifndef FILE_LIBSRPC_DLIST_H
#define FILE_LIBSRPC_DLIST_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <pthread.h>

#define DL_MAGIC 0xD157C0DEu

/*
 * Элемент двухсвязного списка.
 *
 * Предназначен для встраивания в пользовательские структуры:
 *
 * typedef struct {
 *     dlist_elem_t link;
 *     int          id;
 * } my_item_t;
 *
 * ВАЖНО: все элементы и голова должны находиться в одной shared memory
 * области, отображенной во всех процессах по одному и тому же адресу.
 */
typedef struct dlist_elem {
    struct dlist_elem *prev;
    struct dlist_elem *next;
} dlist_elem_t;

/*
 * Голова списка.
 *
 * anchor - фиктивный элемент, замыкающий список.
 * Если список пуст:
 *     anchor.next == anchor.prev == &anchor
 *
 * tx_state, tx_node, old_prev, old_next - журнал для отката транзакции,
 * если владелец mutex умер во время добавления/удаления.
 *
 * self нужен для проверки, что shared memory отображена по тому же адресу,
 * по которому список был инициализирован.
 */
typedef struct dlist_head {
    uint32_t             magic;
    uint32_t             version;
    struct dlist_head   *self;

    pthread_mutex_t      mutex;

    dlist_elem_t         anchor;

    uint32_t             tx_state;
    uint32_t             pad;

    dlist_elem_t        *tx_node;
    dlist_elem_t        *old_prev;
    dlist_elem_t        *old_next;
} dlist_head_t;

/*
 * Получить структуру, в которую встроен элемент.
 *
 * ptr    - указатель на dlist_elem_t
 * type   - тип пользовательской структуры
 * member - имя поля dlist_elem_t внутри этой структуры
 */
#define DL_ENTRY(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/*
 * Проверка, что elem является реальным элементом списка,
 * а не NULL и не anchor.
 */
 #define DL_ELEM_VALID(head, elem) \
 ((elem) != NULL && (elem) != &(head)->anchor)

/*
* Вернуть next, если elem валиден, иначе NULL.
*/
#define DL_NEXT_ELEM_IF_VALID(head, elem) \
 (DL_ELEM_VALID(head, elem) ? (elem)->next : NULL)

/*
* Вернуть указатель на пользовательскую структуру, если elem валиден.
* Если elem == NULL или elem == anchor, вернется NULL.
*/
#define DL_ENTRY_IF_VALID(head, elem, type, member) \
 (DL_ELEM_VALID(head, elem) ? DL_ENTRY((elem), type, member) : NULL)

/*
* Перебор dlist_elem_t*.
*
* Использование:
*
* dlist_elem_t *e;
* DL_FOR_EACH_ELEM(head, e) {
*     ...
* }
*
* ВАЖНО: не удаляйте текущий элемент из списка внутри этого цикла,
* если хотите продолжить итерацию. Для удаления используйте safe-вариант.
*/
#define DL_FOR_EACH_ELEM(head, elem) \
 for (dlist_elem_t *(elem) = (head)->anchor.next; \
      DL_ELEM_VALID(head, elem); \
      (elem) = DL_NEXT_ELEM_IF_VALID(head, elem))

/*
* Перебор пользовательских структур.
*
* pos имеет тип type* и объявляется внутри макроса.
*
* Использование:
*
* DL_FOR_EACH_ENTRY(head, it, item_t, link) {
*     printf("id=%d\n", it->id);
* }
*/
#define DL_FOR_EACH_ENTRY(head, pos, type, member) \
 for (type *(pos) = DL_ENTRY_IF_VALID(head, (head)->anchor.next, type, member); \
      (pos) != NULL; \
      (pos) = DL_ENTRY_IF_VALID(head, DL_NEXT_ELEM_IF_VALID(head, &(pos)->member), type, member))

/*
* Safe-перебор dlist_elem_t*.
*
* Можно удалять текущий элемент из списка во время итерации.
*
* Использование:
*
* dlist_elem_t *e, *tmp;
* DL_FOR_EACH_ELEM_SAFE(head, e, tmp) {
*     if (need_remove(e))
*         dlist_remove_locked(head, e);
* }
*/
#define DL_FOR_EACH_ELEM_SAFE(head, elem, tmp) \
 for (dlist_elem_t *(elem) = (head)->anchor.next, \
                     *(tmp) = DL_NEXT_ELEM_IF_VALID(head, (head)->anchor.next); \
      DL_ELEM_VALID(head, elem); \
      (elem) = (tmp), \
      (tmp) = DL_NEXT_ELEM_IF_VALID(head, elem))

/*
* Safe-перебор пользовательских структур.
*
* pos и tmp имеют тип type* и объявляются внутри макроса.
*
* Использование:
*
* DL_FOR_EACH_ENTRY_SAFE(head, it, tmp, item_t, link) {
*     if (it->id == 42)
*         dlist_remove_locked(head, &it->link);
* }
*/
#define DL_FOR_EACH_ENTRY_SAFE(head, pos, tmp, type, member) \
 for (type *(pos) = DL_ENTRY_IF_VALID(head, (head)->anchor.next, type, member), \
           *(tmp) = DL_ENTRY_IF_VALID(head, DL_NEXT_ELEM_IF_VALID(head, (head)->anchor.next), type, member); \
      (pos) != NULL; \
      (pos) = (tmp), \
           (tmp) = DL_ENTRY_IF_VALID(head, DL_NEXT_ELEM_IF_VALID(head, (pos) ? &(pos)->member : NULL), type, member))

/*
* Короткие псевдонимы.
*/
#define DL_FOREACH(head, pos, type, member) \
 DL_FOR_EACH_ENTRY(head, pos, type, member)

#define DL_FOREACH_SAFE(head, pos, tmp, type, member) \
 DL_FOR_EACH_ENTRY_SAFE(head, pos, tmp, type, member)

/*
 * Инициализация головы списка.
 * Вызывать только один раз, обычно в процессе-создателе shared memory.
 */
int dlist_init(dlist_head_t *h);

/*
 * Проверка, что голова похожа на инициализированный список и отображена
 * по тому же виртуальному адресу.
 */
int dlist_open(dlist_head_t *h);

/*
 * Захват mutex списка с обработкой EOWNERDEAD.
 *
 * Если предыдущий владелец умер, удерживая mutex:
 *   1) вызывается pthread_mutex_consistent();
 *   2) откатывается незавершенная транзакция;
 *   3) функция возвращает 0.
 */
int dlist_lock(dlist_head_t *h);

int dlist_unlock(dlist_head_t *h);

/*
 * Откат незавершенной транзакции.
 * Вызывать только уже удерживая mutex.
 */
void dlist_recover_locked(dlist_head_t *h);

/*
 * Инициализация элемента перед первым добавлением.
 */
void dlist_elem_init(dlist_elem_t *e);

/*
 * Функции с автоматическим захватом/освобождением mutex.
 */
int dlist_push_front(dlist_head_t *h, dlist_elem_t *e);
int dlist_push_back(dlist_head_t *h, dlist_elem_t *e);
int dlist_pop_front(dlist_head_t *h, dlist_elem_t **out);
int dlist_remove(dlist_head_t *h, dlist_elem_t *e);
int dlist_is_empty(dlist_head_t *h, int *out_empty);

/*
 * Функции для вызова уже под заблокированным mutex.
 *
 * Они полезны, если нужно выполнить несколько операций за одну блокировку.
 * Но откат транзакции здесь рассчитан на одну операцию вставки/удаления.
 */
int dlist_push_front_locked(dlist_head_t *h, dlist_elem_t *e);
int dlist_push_back_locked(dlist_head_t *h, dlist_elem_t *e);
int dlist_pop_front_locked(dlist_head_t *h, dlist_elem_t **out);
int dlist_remove_locked(dlist_head_t *h, dlist_elem_t *e);

#endif // FILE_LIBSRPC_DLIST_H


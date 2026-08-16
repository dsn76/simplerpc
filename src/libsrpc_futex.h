/* Qwen3.8-Max */
#ifndef FILE_LIBSRPC_FUTEX_H
#define FILE_LIBSRPC_FUTEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JOB_FUTEX_INFINITE можно передать в job_futex_client_wait(),
 * чтобы ждать бесконечно.
 *
 * timeout_us == 0 означает non-blocking проверку:
 * - если уже DONE, вернётся 0;
 * - иначе вернётся -ETIMEDOUT.
 */
#define JOB_FUTEX_INFINITE UINT64_MAX

enum {
    JOB_FUTEX_STATE_INVALID   = 0,
    JOB_FUTEX_STATE_PENDING   = 1,
    JOB_FUTEX_STATE_DONE      = 2,
    JOB_FUTEX_STATE_CANCELED  = 3,
    JOB_FUTEX_STATE_ERROR     = 4,
    JOB_FUTEX_STATE_DESTROYED = 5
};

/*
 * Структура должна находиться в shared memory.
 *
 * Память под структуру выделяет приложение.
 * Перед первым вызовом job_futex_init() область должна быть нулевой
 * или, как минимум, не содержать активный/повреждённый job_futex_t.
 *
 * Поле state является futex-word.
 */
typedef struct job_futex {
    uint32_t magic;
    uint32_t version;

    /*
     * Это futex. Заказчик спит на этом поле, пока оно равно
     * JOB_FUTEX_STATE_PENDING.
     */
    uint32_t state;

    /*
     * Счётчик исполнителей, вызвавших job_futex_worker_done().
     * Ответы исполнителей здесь не хранятся.
     */
    uint32_t completed;

    /*
     * Количество исполнителей, которые должны завершить работу.
     * Фиксируется при инициализации.
     */
    uint32_t worker_count;

    /*
     * Generation полезен, если одна и та же область памяти может
     * переиспользоваться под новый запрос после timeout/cancel/destroy.
     *
     * Исполнитель может запомнить generation при получении запроса
     * и затем вызвать job_futex_worker_done_gen().
     */
    uint32_t generation;

    uint32_t reserved[2];
} __attribute__((aligned(8))) job_futex_t;

/*
 * Инициализация объекта.
 *
 * worker_count должно быть > 0.
 *
 * Возврат:
 *   0           - успешно;
 *   -EINVAL     - NULL pointer, неверный worker_count или плохое выравнивание;
 *   -EBUSY      - объект уже активен;
 *   -EAGAIN     - объект находится в процессе инициализации;
 *   -EPROTO     - память не похожа на нулевую/нашу структуру, magic/version неверны.
 */
int job_futex_init(job_futex_t *jf, uint32_t worker_count);

/*
 * Деструктор не освобождает shared memory.
 * Он помечает объект DESTROYED и будит возможных ожидающих.
 *
 * После этого приложение может освободить/unmap память.
 */
int job_futex_destroy(job_futex_t *jf);

/*
 * Ожидание заказчиком завершения всех исполнителей.
 *
 * timeout_us:
 *   0                   - не блокироваться;
 *   JOB_FUTEX_INFINITE  - ждать бесконечно;
 *   другое значение     - относительный таймаут в микросекундах.
 *
 * Возврат:
 *   0           - все исполнители завершили работу, state == DONE;
 *   -ETIMEDOUT  - таймаут;
 *   -ECANCELED  - запрос отменён через job_futex_cancel();
 *   -EIO        - запрос переведён в ERROR через job_futex_mark_error();
 *   -ESHUTDOWN  - объект уничтожен через job_futex_destroy();
 *   другие отрицательные errno-коды - ошибки валидации/futex.
 */
int job_futex_client_wait(job_futex_t *jf, uint64_t timeout_us);

/*
 * Исполнитель вызывает эту функцию после завершения своей части работы.
 *
 * ВАЖНО: каждый исполнитель должен вызвать ровно один раз.
 * Модуль не отслеживает идентификаторы исполнителей.
 *
 * Когда последний исполнитель вызовет функцию, state станет DONE,
 * и заказчик будет разбужен.
 */
int job_futex_worker_done(job_futex_t *jf);

/*
 * То же самое, но с проверкой generation.
 *
 * Это полезно при переиспользовании одной и той же shared-области:
 * старый исполнитель, завершившийся после нового job_futex_init(),
 * получит -ESTALE и не сломает новый запрос.
 */
int job_futex_worker_done_gen(job_futex_t *jf, uint32_t generation);

/*
 * Перевести PENDING -> CANCELED, если ещё не завершён.
 * Полезно после таймаута, чтобы поздние исполнители получали -ECANCELED.
 */
int job_futex_cancel(job_futex_t *jf);

/*
 * Перевести PENDING -> ERROR, если нужно разбудить заказчика
 * аварийно, не дожидаясь всех исполнителей.
 */
int job_futex_mark_error(job_futex_t *jf);

/*
 * Вспомогательные функции.
 */
int job_futex_get_state(job_futex_t *jf, uint32_t *state);
int job_futex_get_completed(job_futex_t *jf, uint32_t *completed);
int job_futex_get_generation(job_futex_t *jf, uint32_t *generation);
int job_futex_is_done(job_futex_t *jf, int *done);

/*
 * Строка ошибки. Возвращает strerror(-rc) для отрицательных errno-кодов.
 */
const char *job_futex_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif // FILE_LIBSRPC_FUTEX_H

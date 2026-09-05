/*
 * tlsf_txn.h — публичный API транзакционного TLSF-аллокатора v4.1
 *
 * Назначение
 *   Детерминированный O(1) аллокатор для Linux x86_64 shared memory.
 *   Несколько процессов мапят один пул (желательно по одному виртуальному
 *   адресу: внутри лежат абсолютные указатели).
 *
 * Свойства
 *   - TLSF-сегрегация свободных блоков (матрица FL × SL + битмапы)
 *   - BUL (Binary Undo Log): незавершённая транзакция откатывается при
 *     падении владельца robust-мьютекса (EOWNERDEAD)
 *   - Модель владения: блок жив, пока в uid[] есть хотя бы один владелец
 *   - Двунаправленное слияние (prev_size + флаг BLOCK_PREV_FREE)
 *   - data_type (15 бит) и флаг отложенной очистки dc (1 бит) в sig_dc
 *
 * Контракт ошибок
 *   void-функции: код в tlsf_get_errno() (потоковый tlsf_errno).
 *   int-функции: 0 = успех, < 0 = -TLSF_ERR_*.
 *   Исключения: tlsf_getdc возвращает 0/1 как значение флага;
 *               tlsf_get_data_type возвращает 0…32767 как тип.
 *
 * Формат пула: TLSF_FORMAT_VERSION 0x00040000 (см. tlsf_internal.h).
 */

#ifndef TLSF_TXN_H
#define TLSF_TXN_H

#include <stddef.h>
#include <stdint.h>


/* ── Коды ошибок (положительные константы; int-API возвращает их с минусом) ─ */

#define TLSF_OK                            0
#define TLSF_ERR_LOCK_FAILED               1  /* lock / unlock / consistent */
#define TLSF_ERR_CORRUPTION_UNRECOVERABLE  2  /* откат BUL не восстановил пул */
#define TLSF_ERR_BUL_OVERFLOW              3  /* стек undo переполнен */
#define TLSF_ERR_BLOCK_CORRUPTED           4  /* сигнатура или prev_size */
#define TLSF_ERR_INVALID_PTR               5  /* NULL, вне пула, destructor==NULL */
#define TLSF_ERR_OUT_OF_MEMORY             6  /* нет блока / переполнение calloc */
#define TLSF_ERR_POOL_TOO_SMALL            7  /* размер вне [8KiB, 1TiB] */
#define TLSF_ERR_UID_FULL                  8  /* уже 12 владельцев */
#define TLSF_ERR_UID_NOT_FOUND             9  /* uid нет в заголовке */
#define TLSF_ERR_UID_INVALID              10  /* uid==0, кроме free_uid_blocks */
#define TLSF_ERR_NOT_ALLOCATED            11  /* операция над свободным блоком */
#define TLSF_ERR_BUSY                     12  /* trylock в bul_recover_nb */
#define TLSF_ERR_TOO_MANY_OWNERS          13  /* realloc при >1 владельце */
#define TLSF_ERR_UID_DUPLICATE            14  /* link уже существующего uid */
#define TLSF_ERR_FORMAT_MISMATCH          15  /* чужой format_version */

#define TLSF_MAX_OWNERS   12    /* слотов uid в заголовке; 0 = пустой слот */
#define TLSF_FREE_BATCH   256   /* рекомендация размера пачки у приложения */


typedef struct tlsf_ctrl *tlsf_t;

/*
 * Заголовок физического блока (64 байта). Экспортируется, чтобы деструктор
 * мог прочитать uid[] / признаки блока. Писать поля напрямую нельзя:
 * правки метаданных идут только через API (BUL и/или атомарный sig_dc).
 *
 *   0   size_flags   размер блока (биты ≥2) + BLOCK_FREE / BLOCK_PREV_FREE
 *   8   sig_dc       [63:16] magic48, [15] dc, [14:0] data_type
 *  16   prev_free    связь в свободном списке (валидно, если блок свободен)
 *  24   next_free
 *  32   uid[12]      владельцы; 24 байта = три 8-байтовых слова для BUL
 *  56   prev_size    размер предыдущего физ. блока (для prev-coalesce)
 */
typedef struct block_header {
    uint64_t             size_flags;
    uint64_t             sig_dc;
    struct block_header *prev_free;
    struct block_header *next_free;
    uint16_t             uid[TLSF_MAX_OWNERS];
    uint64_t             prev_size;
} block_header_t;

/*
 * Колбэк обхода tlsf_free_uid_blocks. Вызывается при удерживаемом мьютексе.
 *
 * tlsf       — тот же пул (можно звать tlsf_free_nb / tlsf_cleardc)
 * ptr        — пользовательский указатель (после заголовка)
 * block      — заголовок; только чтение
 * match_uid  — аргумент поиска: 0 в режиме «все с dc==1», иначе целевой uid
 * user       — непрозрачный контекст вызывающего (может быть NULL)
 *
 * Возврат >= 0: продолжить обход с начала кучи (после free структура меняется).
 * Возврат <  0: прервать; код уходит вызывающему, мьютекс уже отпущен.
 *
 * Чтобы не зациклиться при >= 0, блок должен перестать совпадать с критерием
 * (free_nb, cleardc при uid==0) либо нужно вернуть < 0.
 */
typedef int (*tlsf_destructor_fn)(tlsf_t tlsf, void *ptr, block_header_t *block,
                                  uint16_t match_uid, void *user);


/* Инициализация: разметить mem[0..size) как Control+BUL+Heap.
 * mem должен быть доступен всем процессам (mmap SHARED). */
tlsf_t tlsf_create(void *mem, size_t size);
void   tlsf_destroy(tlsf_t tlsf);

/* Явный захват рекурсивного robust-мьютекса. tlsf_lock при EOWNERDEAD
 * откатывает BUL и вызывает pthread_mutex_consistent.
 * Каждому lock — парный unlock того же потока. */
int tlsf_lock(tlsf_t tlsf);
int tlsf_unlock(tlsf_t tlsf);

/* Выделение. uid != 0. data_type маскируется до 15 бит; 0 допустим.
 * Возврат выровнен на 64 байта. NULL + errno при ошибке. */
void *tlsf_malloc(tlsf_t tlsf, size_t size, uint16_t uid, uint16_t data_type);
void *tlsf_calloc(tlsf_t tlsf, size_t count, size_t size, uint16_t uid, uint16_t data_type);

/* Одна транзакция: наследует data_type. Сначала in-place (тот же ptr):
 *   — новый размер влезает в текущий блок (хвост можно отрезать, если ≥ 128);
 *   — иначе поглотить следующий свободный физ. блок, без копирования.
 * Предыдущий сосед не используется: сдвиг указателя или memcpy. Если in-place
 * невозможен — malloc_internal + copy + free_internal, новый блок с dc=0. */
void *tlsf_realloc(tlsf_t tlsf, void *ptr, size_t new_size, uint16_t uid);

/* Снять uid. Блок уходит в пул только когда uid[] пуст. dc/data_type не трогает. */
void  tlsf_free(tlsf_t tlsf, void *ptr, uint16_t uid);

/*
 * Под замком: блок по ptr ещё выделен? Полезный размер (без заголовка).
 * 0 и errno при ошибке. uid нет: отличить «тот же объект» после reuse — задача
 * вызывающего (сигнатура проверяется, но не доказывает тождество).
 */
size_t tlsf_get_block_size(tlsf_t tlsf, void *ptr);

/* Отложенный отказ от владения (под замком, с BUL).
 * 1 владелец: dc=1, uid остаётся, used_bytes без изменения.
 * >1 владелец: снять uid, dc не ставить. uid==0 → UID_INVALID. */
void tlsf_free_dc(tlsf_t tlsf, void *ptr, uint16_t uid);

/* Lock-free доступ к dc / data_type. Без BUL. Гонка TOCTOU/reuse принимается.
 * Все четыре проверяют format_version и что блок выделен. */
int  tlsf_setdc(tlsf_t tlsf, void *ptr);          /* 0 или -ERR */
int  tlsf_cleardc(tlsf_t tlsf, void *ptr);        /* 0 или -ERR */
int  tlsf_getdc(tlsf_t tlsf, void *ptr);          /* 0/1 или -ERR */
int  tlsf_get_data_type(tlsf_t tlsf, void *ptr);  /* 0…32767 или -ERR */

/* Как tlsf_free, но без захвата мьютекса. Вызывающий уже держит замок
 * (tlsf_lock или деструктор free_uid_blocks). После успеха — commit. */
void tlsf_free_nb(tlsf_t tlsf, void *ptr, uint16_t uid);

/* Добавить владельца. Не меняет dc и data_type. */
int tlsf_link(tlsf_t tlsf, void *ptr, uint16_t uid);

/* Физический обход кучи. TLSF сам не free'ит.
 * uid!=0 — блоки с этим владельцем; uid==0 — выделенные с dc==1.
 * destructor==NULL → -INVALID_PTR. user передаётся в каждый вызов. */
int tlsf_free_uid_blocks(tlsf_t tlsf, uint16_t uid,
                         tlsf_destructor_fn destructor, void *user);

/* Ручной откат BUL. recover берёт lock; recover_nb — trylock (−BUSY). */
int tlsf_bul_recover(tlsf_t tlsf);
int tlsf_bul_recover_nb(tlsf_t tlsf);

size_t      tlsf_get_used_size(tlsf_t tlsf);   /* сумма payload выделенных блоков */
size_t      tlsf_get_max_size(tlsf_t tlsf);    /* размер всего пула */
int         tlsf_get_errno(tlsf_t tlsf);       /* последний код текущего потока */
const char *tlsf_strerror(int err);            /* принимает и отрицательные коды */
size_t      tlsf_get_bul_capacity(tlsf_t tlsf);
size_t      tlsf_get_bul_current_usage(tlsf_t tlsf);


#endif /* TLSF_TXN_H */

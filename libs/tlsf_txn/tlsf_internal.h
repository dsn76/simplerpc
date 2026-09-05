/*
 * tlsf_internal.h — Внутренние структуры транзакционного TLSF-аллокатора v4.1
 */

#ifndef TLSF_INTERNAL_H
#define TLSF_INTERNAL_H

#include "tlsf_txn.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>


/* TLSF: первый уровень — floor(log2(size)), индексы 0..40; 2^40 = 1 ТиБ. */
#define FLI_MAX           41
#define SLI_COUNT         4              /* 4 бита второго уровня */
#define SLI_SIZE          (1 << SLI_COUNT) /* 16 подклассов на каждый FL */
#define ALIGN_SIZE        64
#define MIN_USER_SIZE     64             /* мин. payload */
#define MIN_BLOCK_SZ      128            /* заголовок 64 + payload 64 */
#define MAX_BUL_ENTRIES   64             /* запас над worst-case realloc=48 */
#define MAX_POOL_SIZE     ((size_t)1ULL << 40)
#define MIN_POOL_SIZE     ((size_t)8ULL * 1024)
#define TLSF_FORMAT_VERSION 0x00040000U  /* раскладка v4.x; миграции нет */

/* Флаги блока (младшие 2 бита size_flags) */
#define BLOCK_FREE      (1ULL << 0)
#define BLOCK_PREV_FREE (1ULL << 1)
#define FLAG_MASK       0x3ULL
#define SIZE_MASK       (~FLAG_MASK)


/* ═══════════════════════════════════════════════════════════════════════════
 *  Константы для поля sig_dc (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Раскладка sig_dc (8 байт, смещение 8–15):
 *    Биты [63:16] = 48 бит — signature
 *    Бит [15]     = 1 бит  — dc (отложенная очистка)
 *    Биты [14:0]  = 15 бит — data_type
 *
 *  Итого: 48 + 1 + 15 = 64 бита = 8 байт.
 */
#define TLSF_BLOCK_MAGIC48  0xA5C3E7F1B9D2ULL
#define SIG_DC_SIG_SHIFT    16
#define SIG_DC_DC_BIT       (1ULL << 15)
#define SIG_DC_TYPE_MASK    0x7FFFULL


/* ═══════════════════════════════════════════════════════════════════════════
 *  Заголовок блока — 64 байта (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Раскладка:
 *    Смещение  0: size_flags  (8) — размер + флаги
 *    Смещение  8: sig_dc      (8) — signature(48) + dc(1) + data_type(15)
 *    Смещение 16: prev_free   (8) — предыдущий в свободном списке
 *    Смещение 24: next_free   (8) — следующий в свободном списке
 *    Смещение 32: uid[12]     (24) — массив владельцев
 *    Смещение 56: prev_size   (8) — размер предыдущего физ. блока
 *
 *  Итого: 8+8+8+8+24+8 = 64 байта. Определение — в tlsf_txn.h.
 */
_Static_assert(sizeof(block_header_t) == 64,
    "Заголовок блока должен быть ровно 64 байта");


/* ═══════════════════════════════════════════════════════════════════════════
 *  Запись BUL (Binary Undo Log)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Одна запись undo: перед записью 8-байтового слова в SHM кладём старое значение.
 * Откат — LIFO по stack_top. */
typedef struct {
    void    *addr;
    uint64_t old_value;
} bul_record_t;


/* ═══════════════════════════════════════════════════════════════════════════
 *  Контекст транзакции
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    _Atomic uint32_t stack_top; /* число живых записей BUL; 0 = закоммичено */
    pid_t            owner_pid; /* getpid() последнего успешного lock */
    uint64_t         txn_epoch; /* монотонно растёт на каждом commit */
} txn_context_t;


/* ═══════════════════════════════════════════════════════════════════════════
 *  Контрольная структура аллокатора (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Лежит в начале пользовательского буфера. BUL — массив внутри этой структуры
 * (фиксированный размер), куча начинается сразу после неё с выравниванием 64. */
struct tlsf_ctrl {
    uint32_t        format_version;
    uint32_t        _reserved;           /* выравнивание до 8 байт */
    block_header_t *matrix[FLI_MAX][SLI_SIZE]; /* головы свободных списков */
    uint64_t        fl_bitmap;           /* бит fl: есть хотя бы один блок */
    uint64_t        sl_bitmaps[FLI_MAX]; /* бит sl внутри класса fl */
    pthread_mutex_t mutex;               /* recursive + robust + pshared */
    txn_context_t   txn;
    bul_record_t    bul[MAX_BUL_ENTRIES];
    void           *pool_start;
    size_t          pool_size;
    void           *heap_start;
    size_t          heap_size;
    size_t          used_bytes;          /* сумма payload выделенных блоков */
    int             last_errno;          /* не используется как источник errno */
    bool            debug_mode;
};


/* ═══════════════════════════════════════════════════════════════════════════
 *  Инъекция сбоев (для тестирования crash recovery)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef TLSF_INJECT
extern int tlsf_inject_point;
extern int tlsf_inject_counter;
#define TLSF_INJECT_CRASH(name)                                     \
    do {                                                            \
        tlsf_inject_counter++;                                      \
        if (tlsf_inject_point == tlsf_inject_counter) abort();      \
    } while (0)
#else
#define TLSF_INJECT_CRASH(name) ((void)0)
#endif


/* ═══════════════════════════════════════════════════════════════════════════
 *  Inline-утилиты для работы с блоками
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline size_t block_size(const block_header_t *block)
{
    return block->size_flags & SIZE_MASK;
}

static inline bool block_is_free(const block_header_t *block)
{
    return (block->size_flags & BLOCK_FREE) != 0;
}

static inline bool block_prev_free(const block_header_t *block)
{
    return (block->size_flags & BLOCK_PREV_FREE) != 0;
}

static inline void block_set_size(block_header_t *block, size_t size)
{
    block->size_flags = (block->size_flags & FLAG_MASK) | size;
}

static inline block_header_t *block_next(block_header_t *block)
{
    return (block_header_t *)((char *)block + block_size(block));
}

static inline void *block_footer_ptr(block_header_t *block)
{
    return (char *)block + block_size(block) - sizeof(uint64_t);
}

static inline void *ptr_from_block(block_header_t *block)
{
    return (char *)block + sizeof(block_header_t);
}

static inline block_header_t *block_from_ptr(void *ptr)
{
    return (block_header_t *)((char *)ptr - sizeof(block_header_t));
}

static inline size_t block_payload_size(const block_header_t *block)
{
    return block_size(block) - sizeof(block_header_t);
}

static inline size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Inline-утилиты для работы с sig_dc (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  sig_dc layout:
 *    [63:16] signature (48 bit)
 *    [15]    dc (1 bit)
 *    [14:0]  data_type (15 bit)
 */

/* sig_dc читается/пишется только атомарно: lock-free setdc иначе data race с malloc. */
static inline uint64_t load_sig_dc(const block_header_t *hdr)
{
    return atomic_load((_Atomic uint64_t *)&hdr->sig_dc);
}

static inline void store_sig_dc(block_header_t *hdr, uint64_t v)
{
    atomic_store((_Atomic uint64_t *)&hdr->sig_dc, v);
}

static inline bool check_signature(const block_header_t *hdr)
{
    return (load_sig_dc(hdr) >> SIG_DC_SIG_SHIFT) == TLSF_BLOCK_MAGIC48;
}

static inline bool get_dc(const block_header_t *hdr)
{
    return (load_sig_dc(hdr) & SIG_DC_DC_BIT) != 0;
}

static inline uint16_t get_data_type(const block_header_t *hdr)
{
    return (uint16_t)(load_sig_dc(hdr) & SIG_DC_TYPE_MASK);
}

/*
 * make_sig_dc — формирование значения sig_dc из signature, dc и data_type.
 */
static inline uint64_t make_sig_dc(bool dc, uint16_t data_type)
{
    return ((uint64_t)TLSF_BLOCK_MAGIC48 << SIG_DC_SIG_SHIFT)
         | (dc ? SIG_DC_DC_BIT : 0)
         | ((uint64_t)data_type & SIG_DC_TYPE_MASK);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Inline-утилиты для работы с uid[12]
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline int uid_find(const block_header_t *block, uint16_t uid)
{
    for (int i = 0; i < TLSF_MAX_OWNERS; i++) {
        if (block->uid[i] == uid) return i;
    }
    return -1;
}

static inline int uid_find_empty(const block_header_t *block)
{
    for (int i = 0; i < TLSF_MAX_OWNERS; i++) {
        if (block->uid[i] == 0) return i;
    }
    return -1;
}

static inline int uid_count(const block_header_t *block)
{
    int count = 0;
    for (int i = 0; i < TLSF_MAX_OWNERS; i++) {
        if (block->uid[i] != 0) count++;
    }
    return count;
}


#endif /* TLSF_INTERNAL_H */

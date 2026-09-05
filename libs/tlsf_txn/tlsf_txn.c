/*
 * tlsf_txn.c — реализация транзакционного TLSF v4.1
 *
 * Поток типичной операции:
 *   lock_and_recover (EOWNERDEAD → откат BUL)
 *   → мутации метаданных: bul_push_word, затем запись
 *   → bul_commit (stack_top = 0) или при ошибке push — откат внутри bul_push
 *   → unlock
 *
 * tlsf_malloc_internal / tlsf_free_internal не коммитят: так realloc
 * собирает alloc+free в одну транзакцию.
 *
 * Указатели в matrix[] и prev/next_free — абсолютные адреса внутри пула;
 * все процессы должны мапить SHM в совместимые VA.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tlsf_txn.h"
#include "tlsf_internal.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>


/* ═══════════════════════════════════════════════════════════════════════════
 *  Глобальное потоковое состояние
 * ═══════════════════════════════════════════════════════════════════════════ */

__thread int tlsf_errno = TLSF_OK; /* per-thread; tlsf_get_errno читает это */

#ifdef TLSF_INJECT
int tlsf_inject_point = -1;
int tlsf_inject_counter = 0;
#endif


/* ═══════════════════════════════════════════════════════════════════════════
 *  Предварительные объявления
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool bul_recover_internal(struct tlsf_ctrl *ctrl);
static bool validate_control_integrity(struct tlsf_ctrl *ctrl);
static bool validate_block_signature(const block_header_t *block);
static int  lock_and_recover(struct tlsf_ctrl *ctrl);
static bool tlsf_free_internal(struct tlsf_ctrl *ctrl, void *ptr, uint16_t uid);
static void *tlsf_malloc_internal(struct tlsf_ctrl *ctrl, size_t size,
                                  uint16_t uid, uint16_t data_type);
static size_t request_to_block_size(size_t size);


/* ═══════════════════════════════════════════════════════════════════════════
 *  BUL — Binary Undo Log
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Запомнить слово до изменения. Переполнение → полный откат текущей транзакции. */
static inline bool bul_push(struct tlsf_ctrl *ctrl, void *addr, uint64_t old_value)
{
    uint32_t top = atomic_load_explicit(&ctrl->txn.stack_top, memory_order_relaxed);
    if (top >= MAX_BUL_ENTRIES) {
        bul_recover_internal(ctrl);
        tlsf_errno = TLSF_ERR_BUL_OVERFLOW;
        return false;
    }
    ctrl->bul[top].addr = addr;
    ctrl->bul[top].old_value = old_value;
    atomic_store_explicit(&ctrl->txn.stack_top, top + 1, memory_order_release);
    return true;
}

static inline bool bul_push_word(struct tlsf_ctrl *ctrl, void *addr)
{
    uint64_t old = atomic_load((_Atomic uint64_t *)addr);
    return bul_push(ctrl, addr, old);
}

/* uid[i] лежит в 8-байтовом слове по смещению 32 + (i/4)*8 (три слова на 12 uid). */
static inline bool bul_push_uid_word(struct tlsf_ctrl *ctrl, block_header_t *block, int uid_index)
{
    uint64_t *word = (uint64_t *)((char *)block + 32 + (uid_index / 4) * 8);
    return bul_push_word(ctrl, word);
}

/* Коммит — сброс вершины стека. Данные в bul[] можно не чистить. */
static inline void bul_commit(struct tlsf_ctrl *ctrl)
{
    atomic_store_explicit(&ctrl->txn.stack_top, 0, memory_order_release);
    ctrl->txn.txn_epoch++;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  TLSF Mapping
 * ═══════════════════════════════════════════════════════════════════════════ */

/* fl = floor(log2(size)), ограничен FLI_MAX-1. */
static inline int mapping_first_level(size_t size)
{
    if (size == 0) return 0;
    int fl = 63 - __builtin_clzl(size);
    if (fl >= FLI_MAX) fl = FLI_MAX - 1;
    return fl;
}

/* Внутри степени двойки — 16 подклассов по старшим битам мантиссы. */
static inline int mapping_second_level(size_t size, int fl)
{
    if (fl < SLI_COUNT) return 0;
    return (int)((size >> (fl - SLI_COUNT + 1)) & (SLI_SIZE - 1));
}

static inline void mapping_search(size_t size, int *fl_out, int *sl_out)
{
    int fl = mapping_first_level(size);
    int sl = mapping_second_level(size, fl);
    *fl_out = fl;
    *sl_out = sl;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Свободные списки
 * ═══════════════════════════════════════════════════════════════════════════ */

static void remove_from_free_list(struct tlsf_ctrl *ctrl, block_header_t *block, int fl, int sl)
{
    TLSF_INJECT_CRASH("remove_from_free_list");
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        ctrl->matrix[fl][sl] = block->next_free;
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    if (ctrl->matrix[fl][sl] == NULL) {
        ctrl->sl_bitmaps[fl] &= ~(1ULL << sl);
        if (ctrl->sl_bitmaps[fl] == 0) {
            ctrl->fl_bitmap &= ~(1ULL << fl);
        }
    }
}

static void insert_into_free_list(struct tlsf_ctrl *ctrl, block_header_t *block, int fl, int sl)
{
    TLSF_INJECT_CRASH("insert_into_free_list");
    block->prev_free = NULL;
    block->next_free = ctrl->matrix[fl][sl];
    if (ctrl->matrix[fl][sl]) {
        ctrl->matrix[fl][sl]->prev_free = block;
    }
    ctrl->matrix[fl][sl] = block;
    ctrl->sl_bitmaps[fl] |= (1ULL << sl);
    ctrl->fl_bitmap |= (1ULL << fl);
}

/* Ищем блок ≥ size: сначала текущий (fl,sl..), затем следующий непустой FL. */
static block_header_t *find_suitable_block(struct tlsf_ctrl *ctrl, int fl, int sl)
{
    uint64_t sl_mask = ctrl->sl_bitmaps[fl] & (~0ULL << sl);
    if (sl_mask) {
        sl = __builtin_ctzll(sl_mask);
        return ctrl->matrix[fl][sl];
    }
    uint64_t fl_mask = ctrl->fl_bitmap & (~0ULL << (fl + 1));
    if (!fl_mask) return NULL;
    fl = __builtin_ctzll(fl_mask);
    sl = __builtin_ctzll(ctrl->sl_bitmaps[fl]);
    return ctrl->matrix[fl][sl];
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Операции с блоками
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Footer дублирует размер свободного блока в последних 8 байтах (классический TLSF). */
static void write_block_footer(block_header_t *block)
{
    uint64_t *footer = (uint64_t *)block_footer_ptr(block);
    *footer = block_size(block);
}

static void mark_block_free(struct tlsf_ctrl *ctrl __attribute__((unused)), block_header_t *block)
{
    block->size_flags |= BLOCK_FREE;
    /* sig_dc не перезаписывается при free (ТЗ 4.6) */
    write_block_footer(block);
}

static void mark_block_allocated(struct tlsf_ctrl *ctrl __attribute__((unused)), block_header_t *block)
{
    TLSF_INJECT_CRASH("mark_block_allocated");
    block->size_flags &= ~BLOCK_FREE;
}

/* Остаток ≥ MIN_BLOCK_SZ становится новым свободным блоком справа. */
static block_header_t *split_block(struct tlsf_ctrl *ctrl, block_header_t *block, size_t size)
{
    TLSF_INJECT_CRASH("split_block");
    size_t total = block_size(block);
    block_header_t *remainder = (block_header_t *)((char *)block + size);

    remainder->size_flags = (total - size) | BLOCK_FREE;
    store_sig_dc(remainder, make_sig_dc(false, 0));
    remainder->prev_free = NULL;
    remainder->next_free = NULL;
    memset(remainder->uid, 0, sizeof(remainder->uid));
    remainder->prev_size = 0;

    block_set_size(block, size);

    block_header_t *next = block_next(remainder);
    if (next < (block_header_t *)((char *)ctrl->pool_start + ctrl->pool_size)) {
        next->size_flags |= BLOCK_PREV_FREE;
        next->prev_size = block_size(remainder);
    }

    write_block_footer(remainder);

    int fl = mapping_first_level(block_size(remainder));
    int sl = mapping_second_level(block_size(remainder), fl);
    insert_into_free_list(ctrl, remainder, fl, sl);
    return remainder;
}

/* Слияние со следующим свободным соседом. block уже не в свободном списке. */
static void coalesce_next(struct tlsf_ctrl *ctrl, block_header_t *block)
{
    block_header_t *next = block_next(block);
    size_t next_size = block_size(next);
    int fl = mapping_first_level(next_size);
    int sl = mapping_second_level(next_size, fl);
    remove_from_free_list(ctrl, next, fl, sl);
    block_set_size(block, block_size(block) + next_size);
    block->size_flags &= ~BLOCK_PREV_FREE;
    block_header_t *after = block_next(block);
    if (after < (block_header_t *)((char *)ctrl->pool_start + ctrl->pool_size)) {
        after->size_flags |= BLOCK_PREV_FREE;
        after->prev_size = block_size(block);
    }
}

static bool validate_prev_size(struct tlsf_ctrl *ctrl, block_header_t *block)
{
    uint64_t ps = block->prev_size;
    if (ps == 0) return false;
    if (ps % ALIGN_SIZE != 0) return false;
    if (ps > ctrl->pool_size) return false;
    char *prev_addr = (char *)block - ps;
    if (prev_addr < (char *)ctrl->pool_start) return false;
    if (prev_addr >= (char *)ctrl->pool_start + ctrl->pool_size) return false;
    return true;
}

/* При битом prev_size слияние пропускаем: лучше фрагментация, чем порча списков. */
static block_header_t *coalesce_prev(struct tlsf_ctrl *ctrl, block_header_t *block)
{
    if (!block_prev_free(block)) return block;
    if (!validate_prev_size(ctrl, block)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        return block;
    }
    block_header_t *prev = (block_header_t *)((char *)block - block->prev_size);
    if (!validate_block_signature(prev)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        return block;
    }
    if (!block_is_free(prev)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        return block;
    }
    int fl = mapping_first_level(block_size(prev));
    int sl = mapping_second_level(block_size(prev), fl);
    remove_from_free_list(ctrl, prev, fl, sl);
    block_set_size(prev, block_size(prev) + block_size(block));
    return prev;
}

static bool validate_block_signature(const block_header_t *block)
{
    return check_signature(block);
}

static bool validate_pointer(struct tlsf_ctrl *ctrl, const void *ptr)
{
    const char *p = (const char *)ptr;
    const char *start = (const char *)ctrl->pool_start;
    const char *end = start + ctrl->pool_size;
    return p >= start && p < end;
}

static bool validate_control_integrity(struct tlsf_ctrl *ctrl __attribute__((unused)))
{
    return true;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  BUL Recovery: LIFO. Store атомарный, чтобы не драться с setdc по sig_dc.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool bul_recover_internal(struct tlsf_ctrl *ctrl)
{
    uint32_t top = atomic_load_explicit(&ctrl->txn.stack_top, memory_order_acquire);
    if (top == 0) return true;
    if (top > MAX_BUL_ENTRIES) return false;
    while (top > 0) {
        top--;
        bul_record_t *rec = &ctrl->bul[top];
        if (!validate_pointer(ctrl, rec->addr)) return false;
        atomic_store((_Atomic uint64_t *)rec->addr, rec->old_value);
    }
    atomic_store_explicit(&ctrl->txn.stack_top, 0, memory_order_release);
    return validate_control_integrity(ctrl);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Захват mutex с auto-recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Общий вход под замок для публичных операций (malloc/free/link/…). */
static int lock_and_recover(struct tlsf_ctrl *ctrl)
{
    if (ctrl->format_version != TLSF_FORMAT_VERSION) {
        tlsf_errno = TLSF_ERR_FORMAT_MISMATCH;
        return TLSF_ERR_FORMAT_MISMATCH;
    }
    int rc = pthread_mutex_lock(&ctrl->mutex);
    if (rc == EOWNERDEAD) {
        if (!bul_recover_internal(ctrl)) {
            pthread_mutex_unlock(&ctrl->mutex);
            tlsf_errno = TLSF_ERR_CORRUPTION_UNRECOVERABLE;
            return TLSF_ERR_CORRUPTION_UNRECOVERABLE;
        }
        rc = pthread_mutex_consistent(&ctrl->mutex);
    }
    if (rc != 0) {
        tlsf_errno = TLSF_ERR_LOCK_FAILED;
        return TLSF_ERR_LOCK_FAILED;
    }
    ctrl->txn.owner_pid = getpid();
    tlsf_errno = TLSF_OK;
    return TLSF_OK;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Вспомогательные функции
 * ═══════════════════════════════════════════════════════════════════════════ */

static block_header_t *get_first_block(struct tlsf_ctrl *ctrl)
{
    return (block_header_t *)ctrl->heap_start;
}

static block_header_t *get_heap_end(struct tlsf_ctrl *ctrl)
{
    return (block_header_t *)((char *)ctrl->heap_start + ctrl->heap_size);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_create
 * ═══════════════════════════════════════════════════════════════════════════ */

tlsf_t tlsf_create(void *mem, size_t size)
{
    if (!mem) return NULL;
    if (size < MIN_POOL_SIZE) { tlsf_errno = TLSF_ERR_POOL_TOO_SMALL; return NULL; }
    if (size > MAX_POOL_SIZE) { tlsf_errno = TLSF_ERR_POOL_TOO_SMALL; return NULL; }

    memset(mem, 0, size);
    struct tlsf_ctrl *ctrl = (struct tlsf_ctrl *)mem;
    ctrl->format_version = TLSF_FORMAT_VERSION;
    ctrl->_reserved = 0;
    ctrl->pool_start = mem;
    ctrl->pool_size = size;
    ctrl->used_bytes = 0;
    ctrl->last_errno = TLSF_OK;
    ctrl->debug_mode = false;

    /* pshared: один мьютекс на все процессы; robust: EOWNERDEAD; recursive: деструктор. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ctrl->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    atomic_store_explicit(&ctrl->txn.stack_top, 0, memory_order_release);
    ctrl->txn.owner_pid = 0;
    ctrl->txn.txn_epoch = 0;

    /* Куча сразу за tlsf_ctrl, выровнять на 64: указатели пользователя тоже 64. */
    uintptr_t heap_start = (uintptr_t)mem + sizeof(struct tlsf_ctrl);
    heap_start = (heap_start + ALIGN_SIZE - 1) & ~(uintptr_t)(ALIGN_SIZE - 1);
    ctrl->heap_start = (void *)heap_start;
    ctrl->heap_size = size - ((char *)ctrl->heap_start - (char *)mem);
    ctrl->heap_size &= ~(size_t)(ALIGN_SIZE - 1);

    block_header_t *first = get_first_block(ctrl);
    first->size_flags = ctrl->heap_size | BLOCK_FREE;
    store_sig_dc(first, make_sig_dc(false, 0));
    first->prev_free = NULL;
    first->next_free = NULL;
    memset(first->uid, 0, sizeof(first->uid));
    first->prev_size = 0;
    write_block_footer(first);

    block_header_t *next = block_next(first);
    if (next < get_heap_end(ctrl)) {
        next->size_flags = BLOCK_PREV_FREE;
        store_sig_dc(next, make_sig_dc(false, 0));
        next->prev_size = block_size(first);
    }

    int fl = mapping_first_level(block_size(first));
    int sl = mapping_second_level(block_size(first), fl);
    insert_into_free_list(ctrl, first, fl, sl);

    tlsf_errno = TLSF_OK;
    return ctrl;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

void tlsf_destroy(tlsf_t tlsf)
{
    if (!tlsf) return;
    pthread_mutex_destroy(&tlsf->mutex);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_lock / tlsf_unlock (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

int tlsf_lock(tlsf_t tlsf)
{
    if (!tlsf) return -TLSF_ERR_INVALID_PTR;
    if (tlsf->format_version != TLSF_FORMAT_VERSION) {
        return -TLSF_ERR_FORMAT_MISMATCH;
    }
    int rc = pthread_mutex_lock(&tlsf->mutex);
    if (rc == EOWNERDEAD) {
        if (!bul_recover_internal(tlsf)) {
            pthread_mutex_unlock(&tlsf->mutex);
            tlsf_errno = TLSF_ERR_CORRUPTION_UNRECOVERABLE;
            return -TLSF_ERR_CORRUPTION_UNRECOVERABLE;
        }
        rc = pthread_mutex_consistent(&tlsf->mutex);
    }
    if (rc != 0) {
        tlsf_errno = TLSF_ERR_LOCK_FAILED;
        return -TLSF_ERR_LOCK_FAILED;
    }
    tlsf->txn.owner_pid = getpid();
    tlsf_errno = TLSF_OK;
    return 0;
}

int tlsf_unlock(tlsf_t tlsf)
{
    if (!tlsf) return -TLSF_ERR_INVALID_PTR;
    int rc = pthread_mutex_unlock(&tlsf->mutex);
    if (rc != 0) return -TLSF_ERR_LOCK_FAILED;
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_malloc_internal — замок удерживается, commit делает обёртка
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Замок уже взят. Не коммитит: вызывающий делает bul_commit или откат. */
static void *tlsf_malloc_internal(struct tlsf_ctrl *tlsf, size_t size,
                                  uint16_t uid, uint16_t data_type)
{
    if (size == 0) return NULL;

    size_t total = request_to_block_size(size);

    int fl, sl;
    mapping_search(total, &fl, &sl);

    block_header_t *block = find_suitable_block(tlsf, fl, sl);
    if (!block) {
        tlsf_errno = TLSF_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (!validate_block_signature(block)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        return NULL;
    }

    int block_fl = mapping_first_level(block_size(block));
    int block_sl = mapping_second_level(block_size(block), block_fl);
    remove_from_free_list(tlsf, block, block_fl, block_sl);

    size_t block_total = block_size(block);
    bool did_split = (block_total - total >= MIN_BLOCK_SZ);
    if (did_split) {
        split_block(tlsf, block, total);
    }

    mark_block_allocated(tlsf, block);

    block_header_t *next = block_next(block);
    if (next < get_heap_end(tlsf)) {
        if (!bul_push_word(tlsf, &next->size_flags)) {
            return NULL;
        }
        if (did_split) {
            next->size_flags |= BLOCK_PREV_FREE;
            if (!bul_push_word(tlsf, &next->prev_size)) {
                return NULL;
            }
            next->prev_size = block_size(block);
        } else {
            next->size_flags &= ~BLOCK_PREV_FREE;
        }
    }

    if (!bul_push_word(tlsf, &block->sig_dc)) {
        return NULL;
    }
    store_sig_dc(block, make_sig_dc(false, data_type)); /* dc сбрасывается */

    memset(block->uid, 0, sizeof(block->uid));
    if (!bul_push_uid_word(tlsf, block, 0)) {
        return NULL;
    }
    block->uid[0] = uid;

    size_t useful = block_payload_size(block);
    if (!bul_push_word(tlsf, &tlsf->used_bytes)) {
        return NULL;
    }
    tlsf->used_bytes += useful;

    return ptr_from_block(block);
}

void *tlsf_malloc(tlsf_t tlsf, size_t size, uint16_t uid, uint16_t data_type)
{
    if (!tlsf) return NULL;
    if (uid == 0) { tlsf_errno = TLSF_ERR_UID_INVALID; return NULL; }
    if (size == 0) return NULL;

    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return NULL;

    void *ptr = tlsf_malloc_internal(tlsf, size, uid, data_type);
    if (ptr) {
        TLSF_INJECT_CRASH("malloc_commit");
        bul_commit(tlsf);
    }
    pthread_mutex_unlock(&tlsf->mutex);
    return ptr;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_calloc
 * ═══════════════════════════════════════════════════════════════════════════ */

void *tlsf_calloc(tlsf_t tlsf, size_t count, size_t size, uint16_t uid, uint16_t data_type)
{
    size_t total_size;
    if (__builtin_mul_overflow(count, size, &total_size)) {
        tlsf_errno = TLSF_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    void *ptr = tlsf_malloc(tlsf, total_size, uid, data_type);
    if (ptr) memset(ptr, 0, total_size);
    return ptr;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_free_internal — без mutex и без commit (true → вызывающий коммитит)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tlsf_free_internal(struct tlsf_ctrl *ctrl, void *ptr, uint16_t uid)
{
    if (!ptr) return false;
    block_header_t *block = block_from_ptr(ptr);

    if (!validate_pointer(ctrl, block) || !validate_block_signature(block)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        return false;
    }
    if (block_is_free(block)) {
        tlsf_errno = TLSF_ERR_NOT_ALLOCATED;
        return false;
    }

    int idx = uid_find(block, uid);
    if (idx < 0) {
        tlsf_errno = TLSF_ERR_UID_NOT_FOUND;
        return false;
    }

    for (int i = 0; i < TLSF_MAX_OWNERS; i++) {
        if (block->uid[i] == uid) {
            if (!bul_push_uid_word(ctrl, block, i)) return false;
            block->uid[i] = 0;
        }
    }

    if (uid_count(block) > 0) {
        return true; /* ещё владельцы — блок остаётся выделенным */
    }

    size_t useful = block_payload_size(block);
    if (!bul_push_word(ctrl, &ctrl->used_bytes)) return false;
    ctrl->used_bytes -= useful;

    block_header_t *merged = coalesce_prev(ctrl, block); /* сначала назад */

    block_header_t *next = block_next(merged);
    if (next < get_heap_end(ctrl) && block_is_free(next)) {
        coalesce_next(ctrl, merged); /* затем вперёд: тройное слияние в одной txn */
    }

    mark_block_free(ctrl, merged);

    int fl = mapping_first_level(block_size(merged));
    int sl = mapping_second_level(block_size(merged), fl);
    insert_into_free_list(ctrl, merged, fl, sl);

    block_header_t *after = block_next(merged);
    if (after < get_heap_end(ctrl)) {
        if (!bul_push_word(ctrl, &after->size_flags)) return false;
        after->size_flags |= BLOCK_PREV_FREE;
        if (!bul_push_word(ctrl, &after->prev_size)) return false;
        after->prev_size = block_size(merged);
    }

    TLSF_INJECT_CRASH("free_commit");
    return true;
}

void tlsf_free(tlsf_t tlsf, void *ptr, uint16_t uid)
{
    if (!tlsf || !ptr) return;
    if (uid == 0) { tlsf_errno = TLSF_ERR_UID_INVALID; return; }
    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return;
    if (tlsf_free_internal(tlsf, ptr, uid)) {
        bul_commit(tlsf);
    }
    pthread_mutex_unlock(&tlsf->mutex);
}

void tlsf_free_nb(tlsf_t tlsf, void *ptr, uint16_t uid)
{
    /* Без lock: вызывающий отвечает за взаимное исключение. */
    if (!tlsf || !ptr) return;
    if (uid == 0) { tlsf_errno = TLSF_ERR_UID_INVALID; return; }
    if (tlsf_free_internal(tlsf, ptr, uid)) {
        bul_commit(tlsf);
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_free_dc (отложенная очистка, v4.0)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  uid_count == 1: устанавливает dc=1 через BUL, uid не удаляется.
 *  uid_count > 1: удаляет uid (как обычный free), dc не устанавливается.
 */

void tlsf_free_dc(tlsf_t tlsf, void *ptr, uint16_t uid)
{
    if (!tlsf || !ptr) return;
    if (uid == 0) { tlsf_errno = TLSF_ERR_UID_INVALID; return; }

    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return;

    block_header_t *block = block_from_ptr(ptr);
    if (!validate_pointer(tlsf, block) || !validate_block_signature(block)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        pthread_mutex_unlock(&tlsf->mutex);
        return;
    }
    if (block_is_free(block)) {
        tlsf_errno = TLSF_ERR_NOT_ALLOCATED;
        pthread_mutex_unlock(&tlsf->mutex);
        return;
    }

    int idx = uid_find(block, uid);
    if (idx < 0) {
        tlsf_errno = TLSF_ERR_UID_NOT_FOUND;
        pthread_mutex_unlock(&tlsf->mutex);
        return;
    }

    int owners = uid_count(block);

    if (owners == 1) {
        /* Единственный владелец: устанавливаем dc=1, uid не удаляем */
        if (!bul_push_word(tlsf, &block->sig_dc)) {
            pthread_mutex_unlock(&tlsf->mutex);
            return;
        }
        atomic_fetch_or((_Atomic uint64_t *)&block->sig_dc, SIG_DC_DC_BIT);
    } else {
        /* Несколько владельцев: удаляем uid (как обычный free) */
        for (int i = 0; i < TLSF_MAX_OWNERS; i++) {
            if (block->uid[i] == uid) {
                if (!bul_push_uid_word(tlsf, block, i)) {
                    pthread_mutex_unlock(&tlsf->mutex);
                    return;
                }
                block->uid[i] = 0;
            }
        }
    }

    bul_commit(tlsf);
    pthread_mutex_unlock(&tlsf->mutex);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_setdc / tlsf_getdc (атомарные, без блокировки, v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Общие проверки lock-free API. Между ними и RMW блок могут освободить/перевыделить. */
static int dc_precheck(tlsf_t tlsf, void *ptr, block_header_t **out)
{
    if (!tlsf || !ptr) return -TLSF_ERR_INVALID_PTR;
    if (tlsf->format_version != TLSF_FORMAT_VERSION) {
        return -TLSF_ERR_FORMAT_MISMATCH;
    }
    block_header_t *block = block_from_ptr(ptr);
    if (!validate_pointer(tlsf, block)) return -TLSF_ERR_INVALID_PTR;
    if (!check_signature(block)) return -TLSF_ERR_BLOCK_CORRUPTED;
    if (block_is_free(block)) return -TLSF_ERR_NOT_ALLOCATED;
    *out = block;
    return 0;
}

int tlsf_setdc(tlsf_t tlsf, void *ptr)
{
    block_header_t *block;
    int rc = dc_precheck(tlsf, ptr, &block);
    if (rc != 0) return rc;
    atomic_fetch_or((_Atomic uint64_t *)&block->sig_dc, SIG_DC_DC_BIT);
    return 0;
}

int tlsf_cleardc(tlsf_t tlsf, void *ptr)
{
    block_header_t *block;
    int rc = dc_precheck(tlsf, ptr, &block);
    if (rc != 0) return rc;
    atomic_fetch_and((_Atomic uint64_t *)&block->sig_dc, ~SIG_DC_DC_BIT);
    return 0;
}

int tlsf_getdc(tlsf_t tlsf, void *ptr)
{
    block_header_t *block;
    int rc = dc_precheck(tlsf, ptr, &block);
    if (rc != 0) return rc;
    return get_dc(block) ? 1 : 0;
}

int tlsf_get_data_type(tlsf_t tlsf, void *ptr)
{
    block_header_t *block;
    int rc = dc_precheck(tlsf, ptr, &block);
    if (rc != 0) return rc;
    return (int)get_data_type(block);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_realloc (наследует data_type)
 * ═══════════════════════════════════════════════════════════════════════════ */

static size_t request_to_block_size(size_t size)
{
    size_t payload = align_up(size, ALIGN_SIZE);
    if (payload < MIN_USER_SIZE) payload = MIN_USER_SIZE;
    size_t total = align_up(payload + sizeof(block_header_t), ALIGN_SIZE);
    if (total < MIN_BLOCK_SZ) total = MIN_BLOCK_SZ;
    return total;
}

static int realloc_try_inplace(struct tlsf_ctrl *ctrl, block_header_t *block, size_t needed)
{
    size_t cur = block_size(block);
    size_t old_payload = block_payload_size(block);
    block_header_t *end = get_heap_end(ctrl);

    if (needed <= cur) {
        if (cur - needed >= MIN_BLOCK_SZ) {
            if (!bul_push_word(ctrl, &block->size_flags)) return -1;
            if (!bul_push_word(ctrl, &ctrl->used_bytes)) return -1;
            split_block(ctrl, block, needed);
            ctrl->used_bytes -= old_payload - block_payload_size(block);
        }
        return 1;
    }

    block_header_t *next = block_next(block);
    if (next >= end) return 0;
    if (!validate_block_signature(next) || !block_is_free(next)) return 0;

    size_t nsz = block_size(next);
    if (nsz == 0 || nsz % ALIGN_SIZE != 0) return 0;
    size_t combined = cur + nsz;
    if (combined < needed) return 0;

    int fl = mapping_first_level(nsz);
    int sl = mapping_second_level(nsz, fl);
    remove_from_free_list(ctrl, next, fl, sl);

    if (!bul_push_word(ctrl, &block->size_flags)) return -1;
    if (!bul_push_word(ctrl, &ctrl->used_bytes)) return -1;

    block_set_size(block, combined);

    if (combined - needed >= MIN_BLOCK_SZ) {
        split_block(ctrl, block, needed);
    } else {
        block_header_t *after = block_next(block);
        if (after < end) {
            if (!bul_push_word(ctrl, &after->size_flags)) return -1;
            if (!bul_push_word(ctrl, &after->prev_size)) return -1;
            after->size_flags &= ~BLOCK_PREV_FREE;
        }
    }

    ctrl->used_bytes += block_payload_size(block) - old_payload;
    return 1;
}

/* Одна транзакция: in-place (следующий свободный / ужатие) либо malloc+copy+free. */
void *tlsf_realloc(tlsf_t tlsf, void *ptr, size_t new_size, uint16_t uid)
{
    if (!tlsf) return NULL;
    if (!ptr) return tlsf_malloc(tlsf, new_size, uid, 0);
    if (new_size == 0) {
        tlsf_free(tlsf, ptr, uid);
        return NULL;
    }
    if (uid == 0) { tlsf_errno = TLSF_ERR_UID_INVALID; return NULL; }

    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return NULL;

    block_header_t *block = block_from_ptr(ptr);
    if (!validate_pointer(tlsf, block) || !validate_block_signature(block)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }
    if (block_is_free(block)) {
        tlsf_errno = TLSF_ERR_NOT_ALLOCATED;
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }

    if (uid_find(block, uid) < 0) {
        tlsf_errno = TLSF_ERR_UID_NOT_FOUND;
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }
    if (uid_count(block) > 1) {
        tlsf_errno = TLSF_ERR_TOO_MANY_OWNERS;
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }

    size_t needed = request_to_block_size(new_size);
    int ip = realloc_try_inplace(tlsf, block, needed);
    if (ip < 0) {
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }
    if (ip > 0) {
        TLSF_INJECT_CRASH("realloc_inplace_commit");
        bul_commit(tlsf);
        pthread_mutex_unlock(&tlsf->mutex);
        return ptr;
    }

    uint16_t old_data_type = get_data_type(block);
    size_t old_payload = block_payload_size(block);
    size_t new_payload = needed - sizeof(block_header_t);

    void *new_ptr = tlsf_malloc_internal(tlsf, new_size, uid, old_data_type);
    if (!new_ptr) {
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }

    size_t copy_size = old_payload < new_payload ? old_payload : new_payload;
    memcpy(new_ptr, ptr, copy_size);

    TLSF_INJECT_CRASH("realloc_between");

    if (!tlsf_free_internal(tlsf, ptr, uid)) {
        bul_recover_internal(tlsf);
        pthread_mutex_unlock(&tlsf->mutex);
        return NULL;
    }

    bul_commit(tlsf);
    pthread_mutex_unlock(&tlsf->mutex);
    return new_ptr;
}

size_t tlsf_get_block_size(tlsf_t tlsf, void *ptr)
{
    if (!tlsf || !ptr) {
        tlsf_errno = TLSF_ERR_INVALID_PTR;
        return 0;
    }

    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return 0;

    block_header_t *block = block_from_ptr(ptr);
    if (!validate_pointer(tlsf, block) || !validate_block_signature(block)) {
        tlsf_errno = TLSF_ERR_BLOCK_CORRUPTED;
        pthread_mutex_unlock(&tlsf->mutex);
        return 0;
    }
    if (block_is_free(block)) {
        tlsf_errno = TLSF_ERR_NOT_ALLOCATED;
        pthread_mutex_unlock(&tlsf->mutex);
        return 0;
    }

    size_t result = block_payload_size(block);
    tlsf_errno = TLSF_OK;
    pthread_mutex_unlock(&tlsf->mutex);
    return result;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_link
 * ═══════════════════════════════════════════════════════════════════════════ */

int tlsf_link(tlsf_t tlsf, void *ptr, uint16_t uid)
{
    if (!tlsf || !ptr) return -TLSF_ERR_INVALID_PTR;
    if (uid == 0) return -TLSF_ERR_UID_INVALID;

    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return -rc;

    block_header_t *block = block_from_ptr(ptr);
    if (!validate_pointer(tlsf, block) || !validate_block_signature(block)) {
        pthread_mutex_unlock(&tlsf->mutex);
        return -TLSF_ERR_BLOCK_CORRUPTED;
    }
    if (block_is_free(block)) {
        pthread_mutex_unlock(&tlsf->mutex);
        return -TLSF_ERR_NOT_ALLOCATED;
    }
    if (uid_find(block, uid) >= 0) {
        pthread_mutex_unlock(&tlsf->mutex);
        return -TLSF_ERR_UID_DUPLICATE;
    }
    int slot = uid_find_empty(block);
    if (slot < 0) {
        pthread_mutex_unlock(&tlsf->mutex);
        return -TLSF_ERR_UID_FULL;
    }
    if (!bul_push_uid_word(tlsf, block, slot)) {
        pthread_mutex_unlock(&tlsf->mutex);
        return -TLSF_ERR_BUL_OVERFLOW;
    }
    block->uid[slot] = uid;
    bul_commit(tlsf);
    pthread_mutex_unlock(&tlsf->mutex);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  tlsf_free_uid_blocks
 *  После деструктора куча могла сжаться (coalesce) — проход только с начала.
 * ═══════════════════════════════════════════════════════════════════════════ */

int tlsf_free_uid_blocks(tlsf_t tlsf, uint16_t uid,
                         tlsf_destructor_fn destructor, void *user)
{
    if (!tlsf) return -TLSF_ERR_INVALID_PTR;
    if (!destructor) return -TLSF_ERR_INVALID_PTR;

    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return -rc;

    bool found;
    do {
        found = false;
        block_header_t *block = get_first_block(tlsf);
        block_header_t *end = get_heap_end(tlsf);

        while (block < end) {
            if (!validate_block_signature(block)) {
                pthread_mutex_unlock(&tlsf->mutex);
                return -TLSF_ERR_BLOCK_CORRUPTED;
            }
            size_t bsz = block_size(block);
            size_t remaining = (size_t)((char *)end - (char *)block);
            if (bsz == 0 || bsz > remaining) {
                pthread_mutex_unlock(&tlsf->mutex);
                return -TLSF_ERR_BLOCK_CORRUPTED;
            }

            if (block_is_free(block)) {
                block = (block_header_t *)((char *)block + bsz);
                continue;
            }

            bool match = (uid == 0) ? get_dc(block) : (uid_find(block, uid) >= 0);
            if (match) {
                found = true;
                int drc = destructor(tlsf, ptr_from_block(block), block, uid, user);
                if (drc < 0) {
                    pthread_mutex_unlock(&tlsf->mutex);
                    return drc;
                }
                break; /* не продолжать по старым смещениям */
            }

            block = (block_header_t *)((char *)block + bsz);
        }
    } while (found);

    pthread_mutex_unlock(&tlsf->mutex);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

int tlsf_bul_recover(tlsf_t tlsf)
{
    if (!tlsf) return -TLSF_ERR_INVALID_PTR;
    if (tlsf->format_version != TLSF_FORMAT_VERSION) {
        return -TLSF_ERR_FORMAT_MISMATCH;
    }
    int rc = pthread_mutex_lock(&tlsf->mutex);
    if (rc == EOWNERDEAD) {
        if (!bul_recover_internal(tlsf)) {
            pthread_mutex_unlock(&tlsf->mutex);
            return -TLSF_ERR_CORRUPTION_UNRECOVERABLE;
        }
        pthread_mutex_consistent(&tlsf->mutex);
        pthread_mutex_unlock(&tlsf->mutex);
        return 0;
    }
    if (rc != 0) return -TLSF_ERR_LOCK_FAILED;
    uint32_t top = atomic_load(&tlsf->txn.stack_top);
    if (top > 0) {
        if (!bul_recover_internal(tlsf)) {
            pthread_mutex_unlock(&tlsf->mutex);
            return -TLSF_ERR_CORRUPTION_UNRECOVERABLE;
        }
    }
    pthread_mutex_unlock(&tlsf->mutex);
    return 0;
}

int tlsf_bul_recover_nb(tlsf_t tlsf)
{
    if (!tlsf) return -TLSF_ERR_INVALID_PTR;
    if (tlsf->format_version != TLSF_FORMAT_VERSION) {
        return -TLSF_ERR_FORMAT_MISMATCH;
    }
    int rc = pthread_mutex_trylock(&tlsf->mutex);
    if (rc == EBUSY) return -TLSF_ERR_BUSY;
    if (rc == EOWNERDEAD) {
        if (!bul_recover_internal(tlsf)) {
            pthread_mutex_unlock(&tlsf->mutex);
            return -TLSF_ERR_CORRUPTION_UNRECOVERABLE;
        }
        pthread_mutex_consistent(&tlsf->mutex);
        pthread_mutex_unlock(&tlsf->mutex);
        return 0;
    }
    if (rc != 0) return -TLSF_ERR_LOCK_FAILED;
    uint32_t top = atomic_load(&tlsf->txn.stack_top);
    if (top > 0) {
        if (!bul_recover_internal(tlsf)) {
            pthread_mutex_unlock(&tlsf->mutex);
            return -TLSF_ERR_CORRUPTION_UNRECOVERABLE;
        }
    }
    pthread_mutex_unlock(&tlsf->mutex);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Статистика и диагностика
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t tlsf_get_used_size(tlsf_t tlsf)
{
    if (!tlsf) return 0;
    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return 0;
    size_t result = tlsf->used_bytes;
    pthread_mutex_unlock(&tlsf->mutex);
    return result;
}

size_t tlsf_get_max_size(tlsf_t tlsf)
{
    if (!tlsf) return 0;
    int rc = lock_and_recover(tlsf);
    if (rc != TLSF_OK) return 0;
    size_t result = tlsf->pool_size;
    pthread_mutex_unlock(&tlsf->mutex);
    return result;
}

int tlsf_get_errno(tlsf_t tlsf)
{
    (void)tlsf;
    return tlsf_errno;
}

const char *tlsf_strerror(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case TLSF_OK:                            return "OK";
    case TLSF_ERR_LOCK_FAILED:               return "Lock failed";
    case TLSF_ERR_CORRUPTION_UNRECOVERABLE:  return "Corruption unrecoverable";
    case TLSF_ERR_BUL_OVERFLOW:              return "BUL overflow";
    case TLSF_ERR_BLOCK_CORRUPTED:           return "Block corrupted";
    case TLSF_ERR_INVALID_PTR:               return "Invalid pointer";
    case TLSF_ERR_OUT_OF_MEMORY:             return "Out of memory";
    case TLSF_ERR_POOL_TOO_SMALL:            return "Pool too small";
    case TLSF_ERR_UID_FULL:                  return "UID array full";
    case TLSF_ERR_UID_NOT_FOUND:             return "UID not found";
    case TLSF_ERR_UID_INVALID:               return "UID invalid (zero)";
    case TLSF_ERR_NOT_ALLOCATED:             return "Block not allocated";
    case TLSF_ERR_BUSY:                      return "Mutex busy";
    case TLSF_ERR_TOO_MANY_OWNERS:           return "Too many owners";
    case TLSF_ERR_UID_DUPLICATE:             return "UID duplicate";
    case TLSF_ERR_FORMAT_MISMATCH:           return "Format version mismatch";
    default: return "Unknown error";
    }
}

size_t tlsf_get_bul_capacity(tlsf_t tlsf)
{
    (void)tlsf;
    return MAX_BUL_ENTRIES;
}

size_t tlsf_get_bul_current_usage(tlsf_t tlsf)
{
    if (!tlsf) return 0;
    return (size_t)atomic_load_explicit(&tlsf->txn.stack_top, memory_order_acquire);
}

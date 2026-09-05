/*
 * test_tlsf_txn.c — Тесты транзакционного TLSF-аллокатора v4.1
 *
 * Полный набор тестов:
 *   1. Функциональные (malloc/free/calloc/realloc + data_type)
 *   2. Модель владения (до 12 owners)
 *   3. free_uid_blocks с деструктором
 *   4. Статистика
 *   5. Recovery
 *   6. Диагностика
 *   7. Граничные
 *   8. Нагрузочные
 *   9. prev-coalesce
 *  10. format_version
 *  11. tlsf_lock / tlsf_unlock (v4.0)
 *  12. tlsf_free_dc (v4.0)
 *  13. tlsf_setdc / tlsf_getdc (v4.0)
 *  14. tlsf_free_nb (v4.0)
 *  15. data_type (v4.0)
 */

#include "tlsf_txn.h"
#include "tlsf_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>


/* ═══════════════════════════════════════════════════════════════════════════
 *  Инфраструктура тестирования
 * ═══════════════════════════════════════════════════════════════════════════ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_BEGIN(name) \
    do { printf("  %-50s ", name); tests_run++; } while (0)

#define TEST_PASS() \
    do { tests_passed++; printf("[PASS]\n"); } while (0)

#define TEST_FAIL(msg) \
    do { printf("[FAIL] %s\n", msg); } while (0)

#define ASSERT(cond, msg) \
    do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)

#define SIZE (128 * 1024)

static void *create_shm(void)
{
    void *mem = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);
    return mem;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Деструкторы для тестов
 * ═══════════════════════════════════════════════════════════════════════════ */




/* ═══════════════════════════════════════════════════════════════════════════
 *  1. Функциональные тесты
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_malloc_free(void)
{
    TEST_BEGIN("malloc/free basic");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    memset(p, 0xAB, 128);
    tlsf_free(t, p, 1);
    ASSERT(tlsf_get_errno(t) == TLSF_OK, "errno after free");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_calloc(void)
{
    TEST_BEGIN("calloc basic");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    int *p = (int *)tlsf_calloc(t, 10, sizeof(int), 1, 0);
    ASSERT(p, "calloc");
    for (int i = 0; i < 10; i++) ASSERT(p[i] == 0, "zeroed");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_realloc(void)
{
    TEST_BEGIN("realloc basic");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    char *p = (char *)tlsf_malloc(t, 64, 1, 0);
    ASSERT(p, "malloc");
    memset(p, 'A', 64);
    char *q = (char *)tlsf_realloc(t, p, 128, 1);
    ASSERT(q, "realloc");
    for (int i = 0; i < 64; i++) ASSERT(q[i] == 'A', "data preserved");
    tlsf_free(t, q, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_realloc_inplace_next(void)
{
    TEST_BEGIN("realloc in-place into next free");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    char *a = (char *)tlsf_malloc(t, 128, 1, 7);
    void *b = tlsf_malloc(t, 128, 2, 0);
    void *c = tlsf_malloc(t, 128, 3, 0);
    ASSERT(a && b && c, "mallocs");
    memset(a, 'Z', 128);
    tlsf_setdc(t, a);
    tlsf_free(t, b, 2);
    char *a2 = (char *)tlsf_realloc(t, a, 200, 1);
    ASSERT(a2 == a, "same pointer");
    for (int i = 0; i < 128; i++) ASSERT(a2[i] == 'Z', "no copy needed");
    ASSERT(tlsf_get_data_type(t, a2) == 7, "type kept");
    ASSERT(tlsf_getdc(t, a2) == 1, "dc kept in-place");
    ASSERT(tlsf_get_block_size(t, a2) >= 200, "grown payload");
    tlsf_free(t, a2, 1);
    tlsf_free(t, c, 3);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_realloc_fallback_copy(void)
{
    TEST_BEGIN("realloc copy when next is allocated");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    char *a = (char *)tlsf_malloc(t, 64, 1, 3);
    void *b = tlsf_malloc(t, 64, 2, 0);
    ASSERT(a && b, "mallocs");
    memset(a, 'Q', 64);
    char *a2 = (char *)tlsf_realloc(t, a, 4096, 1);
    ASSERT(a2, "realloc");
    ASSERT(a2 != a, "moved");
    for (int i = 0; i < 64; i++) ASSERT(a2[i] == 'Q', "copied");
    ASSERT(tlsf_get_data_type(t, a2) == 3, "type inherited");
    ASSERT(tlsf_getdc(t, a2) == 0, "dc cleared on new block");
    tlsf_free(t, a2, 1);
    tlsf_free(t, b, 2);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_get_block_size(void)
{
    TEST_BEGIN("tlsf_get_block_size allocated vs free");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 100, 1, 0);
    ASSERT(p, "malloc");
    size_t sz = tlsf_get_block_size(t, p);
    ASSERT(sz >= 128 && (sz % 64) == 0, "payload aligned");
    tlsf_free(t, p, 1);
    ASSERT(tlsf_get_block_size(t, p) == 0, "gone");
    ASSERT(tlsf_get_errno(t) == TLSF_ERR_NOT_ALLOCATED, "errno");
    ASSERT(tlsf_get_block_size(t, NULL) == 0, "null");
    ASSERT(tlsf_get_errno(t) == TLSF_ERR_INVALID_PTR, "null errno");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_uid_zero_rejected(void)
{
    TEST_BEGIN("uid=0 rejected");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 64, 0, 0);
    ASSERT(p == NULL, "malloc uid=0");
    ASSERT(tlsf_get_errno(t) == TLSF_ERR_UID_INVALID, "errno");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_min_block_size(void)
{
    TEST_BEGIN("min block size = 128");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p1 = tlsf_malloc(t, 1, 1, 0);
    void *p2 = tlsf_malloc(t, 1, 2, 0);
    ASSERT(p1 && p2, "two mallocs");
    ASSERT((char *)p2 - (char *)p1 >= 128, "min block distance");
    tlsf_free(t, p1, 1);
    tlsf_free(t, p2, 2);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_alignment(void)
{
    TEST_BEGIN("alignment = 64");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    for (int i = 0; i < 100; i++) {
        void *p = tlsf_malloc(t, 37 + i, (uint16_t)(i + 1), 0);
        ASSERT(p, "malloc");
        ASSERT(((uintptr_t)p & 63) == 0, "aligned");
        tlsf_free(t, p, (uint16_t)(i + 1));
    }
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_fragmentation(void)
{
    TEST_BEGIN("fragmentation stress");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = tlsf_malloc(t, 128, (uint16_t)(i + 1), 0);
        ASSERT(ptrs[i], "alloc");
    }
    for (int i = 0; i < 64; i += 2) tlsf_free(t, ptrs[i], (uint16_t)(i + 1));
    for (int i = 0; i < 64; i += 2) {
        ptrs[i] = tlsf_malloc(t, 64, (uint16_t)(i + 1), 0);
        ASSERT(ptrs[i], "realloc holes");
    }
    for (int i = 0; i < 64; i++) tlsf_free(t, ptrs[i], (uint16_t)(i + 1));
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  2. Модель владения
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_two_owners(void)
{
    TEST_BEGIN("two owners: free detaches, second frees");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    ASSERT(tlsf_link(t, p, 2) == 0, "link");
    tlsf_free(t, p, 1);
    ASSERT(tlsf_get_errno(t) == TLSF_OK, "first free");
    void *q = tlsf_malloc(t, 64, 3, 0);
    ASSERT(q, "alloc after first free");
    tlsf_free(t, p, 2);
    ASSERT(tlsf_get_errno(t) == TLSF_OK, "second free");
    tlsf_free(t, q, 3);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_link_full_array(void)
{
    TEST_BEGIN("link full array (12 owners) -> error");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    for (int i = 2; i <= 12; i++) ASSERT(tlsf_link(t, p, (uint16_t)i) == 0, "link");
    ASSERT(tlsf_link(t, p, 13) == -TLSF_ERR_UID_FULL, "full");
    for (int i = 1; i <= 12; i++) tlsf_free(t, p, (uint16_t)i);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  3. free_uid_blocks с деструктором (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int g_destructor_count = 0;
static int counting_destructor(tlsf_t tlsf, void *ptr, block_header_t *block,
                               uint16_t match_uid, void *user)
{
    (void)user;
    g_destructor_count++;
    if (match_uid != 0) {
        tlsf_free_nb(tlsf, ptr, match_uid);
        return 0;
    }
    for (int i = 0; i < TLSF_MAX_OWNERS; i++) {
        if (block->uid[i] != 0) {
            tlsf_free_nb(tlsf, ptr, block->uid[i]);
            return 0;
        }
    }
    return -1;
}

static void test_free_uid_blocks_destructor(void)
{
    TEST_BEGIN("free_uid_blocks with destructor");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p1 = tlsf_malloc(t, 128, 1, 0);
    void *p2 = tlsf_malloc(t, 128, 2, 0);
    ASSERT(p1 && p2, "mallocs");
    g_destructor_count = 0;
    int rc = tlsf_free_uid_blocks(t, 1, counting_destructor, NULL);
    ASSERT(rc == 0, "free uid 1");
    ASSERT(g_destructor_count == 1, "destructor called once");
    tlsf_free(t, p2, 2);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_free_uid_blocks_null_destructor(void)
{
    TEST_BEGIN("free_uid_blocks NULL destructor -> error");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    int rc = tlsf_free_uid_blocks(t, 1, NULL, NULL);
    ASSERT(rc == -TLSF_ERR_INVALID_PTR, "null destructor");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  4. Статистика
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_used_size(void)
{
    TEST_BEGIN("tlsf_get_used_size tracks allocations");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    size_t before = tlsf_get_used_size(t);
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(tlsf_get_used_size(t) > before, "used increased");
    tlsf_free(t, p, 1);
    ASSERT(tlsf_get_used_size(t) == before, "used restored");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  5. Recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_bul_recover_empty(void)
{
    TEST_BEGIN("tlsf_bul_recover on clean state");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    ASSERT(tlsf_bul_recover(t) == 0, "recover clean");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_crash_recovery(void)
{
    TEST_BEGIN("crash recovery via fork");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc before fork");
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    waitpid(pid, NULL, 0);
    ASSERT(tlsf_bul_recover(t) == 0, "recovery");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  6. Диагностика
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_strerror(void)
{
    TEST_BEGIN("tlsf_strerror for all error codes");
    ASSERT(tlsf_strerror(TLSF_OK) != NULL, "ok");
    ASSERT(tlsf_strerror(TLSF_ERR_LOCK_FAILED) != NULL, "lock");
    ASSERT(tlsf_strerror(TLSF_ERR_FORMAT_MISMATCH) != NULL, "fmt");
    ASSERT(tlsf_strerror(TLSF_ERR_BUL_OVERFLOW) != NULL, "bul");
    ASSERT(tlsf_strerror(TLSF_ERR_BLOCK_CORRUPTED) != NULL, "block");
    ASSERT(tlsf_strerror(TLSF_ERR_INVALID_PTR) != NULL, "ptr");
    ASSERT(tlsf_strerror(TLSF_ERR_OUT_OF_MEMORY) != NULL, "oom");
    ASSERT(tlsf_strerror(TLSF_ERR_UID_FULL) != NULL, "full");
    ASSERT(tlsf_strerror(TLSF_ERR_UID_NOT_FOUND) != NULL, "notfound");
    ASSERT(tlsf_strerror(TLSF_ERR_UID_INVALID) != NULL, "invalid");
    ASSERT(tlsf_strerror(TLSF_ERR_NOT_ALLOCATED) != NULL, "notalloc");
    ASSERT(tlsf_strerror(TLSF_ERR_TOO_MANY_OWNERS) != NULL, "owners");
    ASSERT(tlsf_strerror(TLSF_ERR_UID_DUPLICATE) != NULL, "dup");
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  7. Граничные тесты
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_boundary_sizes(void)
{
    TEST_BEGIN("boundary alloc sizes");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    size_t sizes[] = {1, 63, 64, 65, 127, 128, 129, 256, 512, 1024, 4096, 8192};
    for (int i = 0; i < 12; i++) {
        void *p = tlsf_malloc(t, sizes[i], (uint16_t)(i + 1), 0);
        ASSERT(p, "alloc");
        ASSERT(((uintptr_t)p & 63) == 0, "aligned");
        tlsf_free(t, p, (uint16_t)(i + 1));
    }
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_pool_too_small(void)
{
    TEST_BEGIN("pool too small rejected");
    char small[4096];
    memset(small, 0, sizeof(small));
    ASSERT(tlsf_create(small, 4096) == NULL, "too small");
    ASSERT(tlsf_get_errno(NULL) == TLSF_ERR_POOL_TOO_SMALL, "errno");
    TEST_PASS();
}

static void test_null_inputs(void)
{
    TEST_BEGIN("NULL inputs handled gracefully");
    tlsf_free(NULL, NULL, 1);
    void *p = tlsf_malloc(NULL, 64, 1, 0);
    ASSERT(p == NULL, "malloc NULL tlsf");
    ASSERT(tlsf_link(NULL, (void *)0x1, 1) == -TLSF_ERR_INVALID_PTR, "link NULL");
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  8. Нагрузочный тест
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_stress(void)
{
    TEST_BEGIN("stress: random alloc/free patterns");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *ptrs[128];
    memset(ptrs, 0, sizeof(ptrs));
    for (int iter = 0; iter < 1000; iter++) {
        int idx = rand() % 128;
        if (ptrs[idx] == NULL) {
            ptrs[idx] = tlsf_malloc(t, 64 + (rand() % 512), (uint16_t)(idx + 1), 0);
        } else {
            tlsf_free(t, ptrs[idx], (uint16_t)(idx + 1));
            ptrs[idx] = NULL;
        }
    }
    for (int i = 0; i < 128; i++) {
        if (ptrs[i]) tlsf_free(t, ptrs[i], (uint16_t)(i + 1));
    }
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  9. prev-coalesce
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_prev_coalesce(void)
{
    TEST_BEGIN("prev-coalesce: two blocks merge");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *a = tlsf_malloc(t, 128, 1, 0);
    void *b = tlsf_malloc(t, 128, 2, 0);
    void *c = tlsf_malloc(t, 128, 3, 0);
    ASSERT(a && b && c, "mallocs");
    size_t a_addr = (size_t)a;
    tlsf_free(t, a, 1);
    tlsf_free(t, b, 2);
    void *d = tlsf_malloc(t, 256, 4, 0);
    ASSERT(d, "malloc after coalesce");
    ASSERT((size_t)d == a_addr, "merged at A's address");
    tlsf_free(t, c, 3);
    tlsf_free(t, d, 4);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_triple_coalesce(void)
{
    TEST_BEGIN("prev-coalesce: triple merge");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *a = tlsf_malloc(t, 128, 1, 0);
    void *b = tlsf_malloc(t, 128, 2, 0);
    void *c = tlsf_malloc(t, 128, 3, 0);
    ASSERT(a && b && c, "mallocs");
    size_t a_addr = (size_t)a;
    tlsf_free(t, a, 1);
    tlsf_free(t, c, 3);
    tlsf_free(t, b, 2);
    void *d = tlsf_malloc(t, 384, 4, 0);
    ASSERT(d, "malloc after triple");
    ASSERT((size_t)d == a_addr, "merged at A's address");
    tlsf_free(t, d, 4);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  10. format_version
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_format_version_write(void)
{
    TEST_BEGIN("format_version written by tlsf_create");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    ASSERT(t->format_version == TLSF_FORMAT_VERSION, "version match");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_format_version_mismatch(void)
{
    TEST_BEGIN("format_version mismatch -> error");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    t->format_version = 0xDEADBEEF;
    ASSERT(tlsf_bul_recover(t) == -TLSF_ERR_FORMAT_MISMATCH, "mismatch");
    t->format_version = TLSF_FORMAT_VERSION;
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  11. tlsf_lock / tlsf_unlock (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_lock_unlock(void)
{
    TEST_BEGIN("tlsf_lock / tlsf_unlock basic");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    ASSERT(tlsf_lock(t) == 0, "lock");
    ASSERT(tlsf_unlock(t) == 0, "unlock");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_recursive_lock(void)
{
    TEST_BEGIN("tlsf_lock recursive");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    ASSERT(tlsf_lock(t) == 0, "lock 1");
    ASSERT(tlsf_lock(t) == 0, "lock 2 (recursive)");
    ASSERT(tlsf_unlock(t) == 0, "unlock 1");
    ASSERT(tlsf_unlock(t) == 0, "unlock 2");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  12. tlsf_free_dc (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_free_dc_single_owner(void)
{
    TEST_BEGIN("free_dc: single owner sets dc");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    tlsf_free_dc(t, p, 1);
    ASSERT(tlsf_get_errno(t) == TLSF_OK, "errno");
    /* dc должен быть установлен */
    ASSERT(tlsf_getdc(t, p) == 1, "dc set");
    /* uid должен сохраниться */
    block_header_t *block = block_from_ptr(p);
    ASSERT(block->uid[0] == 1, "uid preserved");
    /* Блок остаётся выделенным */
    ASSERT(!block_is_free(block), "block still allocated");
    /* Обычный free освобождает */
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_free_dc_multi_owner(void)
{
    TEST_BEGIN("free_dc: multi owner detaches uid");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    tlsf_link(t, p, 2);
    tlsf_free_dc(t, p, 1);
    ASSERT(tlsf_get_errno(t) == TLSF_OK, "errno");
    /* uid=1 удалён, uid=2 остался */
    block_header_t *block = block_from_ptr(p);
    ASSERT(uid_find(block, 1) < 0, "uid 1 removed");
    ASSERT(uid_find(block, 2) >= 0, "uid 2 present");
    tlsf_free(t, p, 2);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  13. tlsf_setdc / tlsf_getdc (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_setdc_getdc(void)
{
    TEST_BEGIN("setdc / getdc basic");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    ASSERT(tlsf_getdc(t, p) == 0, "dc initially 0");
    ASSERT(tlsf_setdc(t, p) == 0, "setdc");
    ASSERT(tlsf_getdc(t, p) == 1, "dc now 1");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_setdc_on_free_block(void)
{
    TEST_BEGIN("setdc on free block -> error");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    tlsf_free(t, p, 1);
    ASSERT(tlsf_setdc(t, p) == -TLSF_ERR_NOT_ALLOCATED, "setdc on free");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  14. tlsf_free_nb (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_free_nb(void)
{
    TEST_BEGIN("tlsf_free_nb under lock");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(p, "malloc");
    tlsf_lock(t);
    tlsf_free_nb(t, p, 1);
    ASSERT(tlsf_get_errno(t) == TLSF_OK, "errno");
    tlsf_unlock(t);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  15. data_type (v4.0)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_data_type(void)
{
    TEST_BEGIN("data_type stored and retrieved");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0x1234);
    ASSERT(p, "malloc");
    ASSERT(tlsf_get_data_type(t, p) == 0x1234, "data_type match");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_data_type_inherited(void)
{
    TEST_BEGIN("realloc inherits data_type");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0x1234);
    ASSERT(p, "malloc");
    void *q = tlsf_realloc(t, p, 256, 1);
    ASSERT(q, "realloc");
    ASSERT(tlsf_get_data_type(t, q) == 0x1234, "data_type inherited");
    ASSERT(tlsf_getdc(t, q) == 0, "dc cleared on new block");
    tlsf_free(t, q, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_data_type_mask(void)
{
    TEST_BEGIN("data_type bit 15 masked");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0xFFFF);
    ASSERT(p, "malloc");
    ASSERT(tlsf_get_data_type(t, p) == 0x7FFF, "bit 15 masked");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_calloc_overflow(void)
{
    TEST_BEGIN("calloc overflow -> OOM");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_calloc(t, (size_t)-1, 2, 1, 0);
    ASSERT(p == NULL, "overflow");
    ASSERT(tlsf_get_errno(t) == TLSF_ERR_OUT_OF_MEMORY, "errno");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_cleardc(void)
{
    TEST_BEGIN("cleardc clears flag");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(tlsf_setdc(t, p) == 0, "setdc");
    ASSERT(tlsf_getdc(t, p) == 1, "set");
    ASSERT(tlsf_cleardc(t, p) == 0, "cleardc");
    ASSERT(tlsf_getdc(t, p) == 0, "cleared");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_getdc_on_free_block(void)
{
    TEST_BEGIN("getdc on free block -> NOT_ALLOCATED");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    tlsf_free(t, p, 1);
    ASSERT(tlsf_getdc(t, p) == -TLSF_ERR_NOT_ALLOCATED, "getdc free");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_setdc_format_mismatch(void)
{
    TEST_BEGIN("setdc format mismatch");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    t->format_version = 0xDEADBEEF;
    ASSERT(tlsf_setdc(t, p) == -TLSF_ERR_FORMAT_MISMATCH, "mismatch");
    t->format_version = TLSF_FORMAT_VERSION;
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_free_dc_uid_zero(void)
{
    TEST_BEGIN("free_dc uid=0 -> UID_INVALID");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    tlsf_free_dc(t, p, 0);
    ASSERT(tlsf_get_errno(t) == TLSF_ERR_UID_INVALID, "errno");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_free_dc_used_bytes(void)
{
    TEST_BEGIN("free_dc does not change used_bytes");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    size_t used = tlsf_get_used_size(t);
    tlsf_free_dc(t, p, 1);
    ASSERT(tlsf_get_used_size(t) == used, "used unchanged");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_free_uid_blocks_dc_mode(void)
{
    TEST_BEGIN("free_uid_blocks uid=0 matches dc");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p1 = tlsf_malloc(t, 128, 1, 0);
    void *p2 = tlsf_malloc(t, 128, 2, 0);
    ASSERT(tlsf_setdc(t, p1) == 0, "setdc");
    g_destructor_count = 0;
    int rc = tlsf_free_uid_blocks(t, 0, counting_destructor, NULL);
    ASSERT(rc == 0, "scan dc");
    ASSERT(g_destructor_count == 1, "only dc block");
    tlsf_free(t, p2, 2);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static int abort_destructor(tlsf_t tlsf, void *ptr, block_header_t *block,
                            uint16_t match_uid, void *user)
{
    (void)tlsf; (void)ptr; (void)block; (void)match_uid; (void)user;
    return -42;
}

static void test_free_uid_blocks_abort(void)
{
    TEST_BEGIN("free_uid_blocks destructor abort");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    int rc = tlsf_free_uid_blocks(t, 1, abort_destructor, NULL);
    ASSERT(rc == -42, "aborted");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

typedef struct {
    void *ptrs[8];
    int n;
} batch_acc_t;

static int batch_destructor(tlsf_t tlsf, void *ptr, block_header_t *block,
                            uint16_t match_uid, void *user)
{
    (void)tlsf; (void)block; (void)match_uid;
    batch_acc_t *b = (batch_acc_t *)user;
    b->ptrs[b->n++] = ptr;
    return -TLSF_ERR_BUSY;
}

static void test_free_uid_blocks_batch(void)
{
    TEST_BEGIN("free_uid_blocks batch via user");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    batch_acc_t acc = {0};
    int rc = tlsf_free_uid_blocks(t, 1, batch_destructor, &acc);
    ASSERT(rc == -TLSF_ERR_BUSY, "interrupt");
    ASSERT(acc.n == 1 && acc.ptrs[0] == p, "collected");
    tlsf_lock(t);
    tlsf_free_nb(t, acc.ptrs[0], 1);
    tlsf_unlock(t);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static int cleardc_destructor(tlsf_t tlsf, void *ptr, block_header_t *block,
                              uint16_t match_uid, void *user)
{
    (void)block; (void)match_uid; (void)user;
    return tlsf_cleardc(tlsf, ptr);
}

static void test_free_uid_blocks_cleardc(void)
{
    TEST_BEGIN("free_uid_blocks uid=0 + cleardc terminates");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(tlsf_setdc(t, p) == 0, "setdc");
    int rc = tlsf_free_uid_blocks(t, 0, cleardc_destructor, NULL);
    ASSERT(rc == 0, "scan done");
    ASSERT(tlsf_getdc(t, p) == 0, "dc cleared, block kept");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_realloc_null_and_zero(void)
{
    TEST_BEGIN("realloc NULL and size 0");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_realloc(t, NULL, 128, 1);
    ASSERT(p, "realloc NULL -> malloc");
    ASSERT(tlsf_get_data_type(t, p) == 0, "data_type 0");
    void *q = tlsf_realloc(t, p, 0, 1);
    ASSERT(q == NULL, "size 0 -> free");
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}

static void test_data_type_zero(void)
{
    TEST_BEGIN("data_type 0 is valid");
    void *mem = create_shm();
    tlsf_t t = tlsf_create(mem, SIZE);
    ASSERT(t, "create");
    void *p = tlsf_malloc(t, 128, 1, 0);
    ASSERT(tlsf_get_data_type(t, p) == 0, "zero type");
    tlsf_free(t, p, 1);
    tlsf_destroy(t);
    munmap(mem, SIZE);
    TEST_PASS();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Точка входа
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("TLSF Transactional Allocator Test Suite v4.1\n");
    printf("============================================\n\n");

    printf("Functional tests:\n");
    test_malloc_free();
    test_calloc();
    test_calloc_overflow();
    test_realloc();
    test_realloc_inplace_next();
    test_realloc_fallback_copy();
    test_realloc_null_and_zero();
    test_get_block_size();
    test_uid_zero_rejected();
    test_min_block_size();
    test_alignment();
    test_fragmentation();

    printf("\nOwner model tests:\n");
    test_two_owners();
    test_link_full_array();

    printf("\nfree_uid_blocks tests:\n");
    test_free_uid_blocks_destructor();
    test_free_uid_blocks_null_destructor();
    test_free_uid_blocks_dc_mode();
    test_free_uid_blocks_abort();
    test_free_uid_blocks_batch();
    test_free_uid_blocks_cleardc();

    printf("\nStatistics tests:\n");
    test_used_size();

    printf("\nRecovery tests:\n");
    test_bul_recover_empty();
    test_crash_recovery();

    printf("\nDiagnostics tests:\n");
    test_strerror();

    printf("\nBoundary tests:\n");
    test_boundary_sizes();
    test_pool_too_small();
    test_null_inputs();

    printf("\nStress tests:\n");
    test_stress();

    printf("\nPrev-coalesce tests:\n");
    test_prev_coalesce();
    test_triple_coalesce();

    printf("\nFormat version tests:\n");
    test_format_version_write();
    test_format_version_mismatch();

    printf("\nLock/unlock tests (v4.0):\n");
    test_lock_unlock();
    test_recursive_lock();

    printf("\nfree_dc tests (v4.0):\n");
    test_free_dc_single_owner();
    test_free_dc_multi_owner();
    test_free_dc_uid_zero();
    test_free_dc_used_bytes();

    printf("\nsetdc/getdc tests (v4.0):\n");
    test_setdc_getdc();
    test_setdc_on_free_block();
    test_getdc_on_free_block();
    test_cleardc();
    test_setdc_format_mismatch();

    printf("\nfree_nb tests (v4.0):\n");
    test_free_nb();

    printf("\ndata_type tests (v4.0):\n");
    test_data_type();
    test_data_type_zero();
    test_data_type_inherited();
    test_data_type_mask();

    printf("\n============================================\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

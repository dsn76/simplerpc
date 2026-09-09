# TLSF Transactional Allocator (v4.1)

A transactional TLSF allocator for **Linux x86_64 shared memory** (C17). Several processes share one pool: allocations are O(1), and if the lock owner dies, the unfinished operation is rolled back with no external watchdog.

Normative spec: `tz_tlsf_shm_v4.1.md`. Pool format `TLSF_FORMAT_VERSION = 0x00040000` — incompatible with 1.x / 2.x / 3.x and with the 4.0 API draft (destructor without `tlsf` / `user`).

---

## Why this exists

Plain TLSF knows nothing about multiple processes or an “operation cut off in the middle”. Here:

1. **Robust recursive mutex** in SHM — mutual exclusion between processes; when the owner dies, the next `lock` gets `EOWNERDEAD`.
2. **BUL** — before every 8-byte metadata word is written, the old value is saved. An uncommitted stack is rolled back.
3. **`uid` owners** — a block returns to the pool only when all owners have detached.
4. **`dc` / `data_type`** — payload type and a “deferred cleanup needed” flag live in the header so a walker can call an external destructor.

Pointers inside the pool are **absolute**. All processes must map the region to **compatible virtual addresses** (the same VA, or at least the same base if offsets are ever introduced — today the base must match).

---

## Build

```bash
make            # test_tlsf_txn
make test       # build + run
make bench      # latency of setdc/getdc and a malloc/free mix
make debug      # -O0 -g -DTLSF_DEBUG
make clean
```

Dependencies: POSIX/glibc only (`pthread`, `stdatomic`).

---

## Pool architecture

The user passes a contiguous buffer to `tlsf_create`. Inside:

```
+------------------------------------------------------------------+
|                     SHARED MEMORY (size bytes)                   |
+----------------------------+-------------------------------------+
| Control Area = tlsf_ctrl   | Heap Area                           |
|  format_version            |  physical block chain               |
|  matrix[FLI][SLI]          |  each: header 64 + payload ≥ 64     |
|  bitmaps                   |  blocks aligned to 64               |
|  robust recursive mutex    |                                     |
|  txn (stack_top, pid)      |                                     |
|  bul[64]                   |                                     |
|  pool bounds / used_bytes  |                                     |
+----------------------------+-------------------------------------+
```

BUL is not a separate mmap — it is an array inside `tlsf_ctrl`. The heap starts immediately after the structure, aligned to 64 bytes.

### Control structure

- **`matrix[41][16]`** — heads of doubly linked lists of free blocks of one size class.
- **`fl_bitmap` / `sl_bitmaps[]`** — “this class has at least one block”; finding a suitable block is `ctz` on the bitmaps, O(1).
- **`mutex`** — `PTHREAD_PROCESS_SHARED` + `ROBUST` + `RECURSIVE`. Recursion is required so the `free_uid_blocks` destructor can call `tlsf_free` / a nested `lock`.
- **`used_bytes`** — the sum of *useful* bytes of allocated blocks (without headers). `tlsf_free_dc` does not decrease this field: the block is still allocated.

### TLSF size classes

`FLI_MAX = 41` (indices 0…40) covers blocks up to 2⁴⁰ bytes (pool up to 1 TiB). The second level is 16 subclasses inside a power of two (`SLI_COUNT = 4`).

Lookup: round the request up to `max(align_up(size+header), 128)` → compute `(fl, sl)` → take the first non-empty list with index ≥, otherwise the next FL.

---

## Block header (64 bytes)

```
off.   field        meaning
  0    size_flags   [63:2] size (multiple of 64), bit0 FREE, bit1 PREV_FREE
  8    sig_dc       [63:16] magic 0xA5C3E7F1B9D2
                    [15]    dc — deferred cleanup
                    [14:0]  data_type (user-defined, 0 is allowed)
 16    prev_free    only if the block is on a free list
 24    next_free
 32    uid[12]      24 bytes = 3 words of 8 bytes (BUL writes a whole word)
 56    prev_size    size of the previous physical block; for prev-coalesce
```

The user pointer is `header + 64`, also aligned to 64.

**`sig_dc` is always read and written atomically** (`atomic_load` / `store` / `fetch_or` / `fetch_and`). Otherwise lock-free `tlsf_setdc` is a data race with `malloc` under C17.

On **`tlsf_free`, `sig_dc` is not wiped**. For the API, `dc` on a free block is undefined: `getdc` / `setdc` / `cleardc` / `get_data_type` return `-TLSF_ERR_NOT_ALLOCATED`. The next `malloc` overwrites the field entirely (`dc = 0`, new `data_type`).

The `block_header_t` type in `tlsf_txn.h` is for the destructor. Fields are **read-only**. Writing them outside the API breaks BUL and integrity.

---

## Ownership model

- `uid == 0` in the array is an empty slot. Through the API `uid = 0` is forbidden, **except** in `tlsf_free_uid_blocks` (there it means “look for `dc == 1`”).
- The same uid is not duplicated in a block (`link` → `UID_DUPLICATE`).
- `tlsf_free(uid)` clears the slot. If other uids remain — the block stays allocated. If the array is empty — physical free + coalesce.
- `tlsf_link` adds an owner and does not change `dc` or `data_type` (including on a block with `dc == 1`).

Maximum 12 owners.

---

## Transactions and BUL

One public metadata mutation ≈ one transaction (exception: `realloc` — alloc+free in one).

```
before writing a word:  bul_push(addr, old)
write
success:                stack_top = 0   (commit)
crash / overflow:       restore words from the stack top downward
```

On overflow the current transaction is rolled back entirely, errno = `BUL_OVERFLOW`.

`setdc` / `cleardc` / `getdc` / `get_data_type` do **not** write to BUL.

Entry estimates (limit 64):

| Operation | Max entries |
|-----------|-------------|
| malloc with split | 21 |
| free, triple merge | 27 |
| free_dc | 1 |
| realloc (one txn) | 48 |
| link | 1 |

---

## Block coalescing

On the last `free`:

1. If the block has `BLOCK_PREV_FREE` — check `prev_size` (multiple of 64, inside the pool, neighbour is free and has a valid signature). Otherwise backward merge is **skipped** (`BLOCK_CORRUPTED` in errno), so the walker does not follow garbage.
2. If the next physical block is free — merge forward.
3. The merged block is inserted into the matrix; the right-hand neighbour gets `PREV_FREE` and `prev_size`.

The order “prev first, then next” yields a triple merge in one transaction.

---

## Locking and recovery

`lock_and_recover` / `tlsf_lock`:

1. `format_version`
2. `pthread_mutex_lock`
3. `EOWNERDEAD` → `bul_recover_internal` → `pthread_mutex_consistent`  
   The new owner's recursion count is 1; it is not inherited from the dead process.
4. `owner_pid = getpid()`

`tlsf_unlock` does not check `format_version`. `EPERM` → `-LOCK_FAILED`.

`tlsf_bul_recover` — blocking manual rollback if `stack_top > 0`.  
`tlsf_bul_recover_nb` — `trylock`; busy → `-BUSY`.

---

## Deferred cleanup

Two different tools.

| | `setdc` / `cleardc` | `free_dc` |
|--|---------------------|-----------|
| Lock | no | yes |
| BUL | no | yes |
| uid | not checked | required, ≠ 0 |
| Several owners | dc may be set | dc is **not** set, only the uid is detached |
| used_bytes | no | no |

Lock-free path: basic checks, then an atomic RMW. Between the check and the operation another process may free+malloc at the same address — the flag may land on a **new** object. This is accepted in the spec.

`tlsf_getdc`: **0 or 1 is data**, not a success code. Errors are negative.  
`tlsf_get_data_type`: **0…32767 is the type**.

---

## `tlsf_free_uid_blocks`

TLSF does **not** call `free` itself. It walks the heap physically; on a match it calls the destructor.

```c
typedef int (*tlsf_destructor_fn)(tlsf_t tlsf, void *ptr, block_header_t *block,
                                  uint16_t match_uid, void *user);
```

- `uid != 0` — blocks with this owner.
- `uid == 0` — allocated blocks with `dc == 1`; the destructor itself chooses which `uid[]` slot to pass to `tlsf_free_nb`.
- Called under the mutex. Immediate free — `tlsf_free_nb` (its own commit per block).
- `>= 0` — walk **from the start of the heap** (after coalesce old offsets are invalid).
- `< 0` — stop; the mutex is already released; the code is returned as-is.

Without `free_nb` / `cleardc` / `< 0` the loop is infinite.

**Batching:** accumulate pointers in `user`, return e.g. `-TLSF_ERR_BUSY`, then outside `tlsf_lock` + a batch of `free_nb`. Between the return and the next lock the pool is unprotected. Do not take an extra `tlsf_lock` inside the destructor: recursion would drop the wrong nesting level.

The recommended application batch size is `TLSF_FREE_BATCH` (256).

---

## Other operations

**`tlsf_malloc` / `calloc`** — uid in `uid[0]`, the rest zeros, `dc = 0`. Overflow of `count*size` in calloc → OOM without taking the lock.

**`tlsf_realloc`** — one transaction, one owner.

1. If the new size fits in the current block (or a tail ≥ 128 can be split off) — same pointer, no copy; `dc`/`data_type` unchanged.
2. Else if the **next** physical block is free and the sum is enough — absorb it (remainder split), again no copy.
3. Else `malloc` + `memcpy` + `free`: the new block inherits `data_type`, `dc = 0`.

`ptr == NULL` → malloc with type 0; `new_size == 0` → free. The previous neighbour is not touched.

**`tlsf_get_block_size(tlsf, ptr)`** — under the lock. Returns the useful size of an allocated block (without the header), or `0` + errno if the block is already gone / the pointer is bad. `uid` is not checked: free+malloc at the same address looks like a “live” block — that is the writer's problem.

**`tlsf_free_nb`** — the same algorithm as `free`, without `lock`. Requires the lock already held.

---

## Limits

| Parameter | Value |
|-----------|-------|
| Pool | 8 KiB … 1 TiB |
| Alignment | 64 bytes |
| Min block | 128 bytes |
| Owners | 12 |
| data_type | 15 bits |
| BUL | 64 × 16 bytes |
| Mutex | recursive + robust, process-shared |

---

## Example

```c
#include "tlsf_txn.h"
#include <sys/mman.h>

void *mem = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
tlsf_t tlsf = tlsf_create(mem, 128 * 1024);

void *buf = tlsf_malloc(tlsf, 256, /* uid */ 1, /* data_type */ 42);
tlsf_link(tlsf, buf, 2);
tlsf_free(tlsf, buf, 1);   /* still live: uid 2 */
tlsf_free(tlsf, buf, 2);

int dtor(tlsf_t t, void *ptr, block_header_t *block,
         uint16_t match_uid, void *user)
{
    (void)block; (void)user;
    if (match_uid)
        tlsf_free_nb(t, ptr, match_uid);
    return 0;
}
tlsf_free_uid_blocks(tlsf, 1, dtor, NULL);

tlsf_destroy(tlsf);
munmap(mem, 128 * 1024);
```

---

## Error codes

`int` API — negative constants (except `getdc` / `get_data_type`). `void` API — `tlsf_get_errno`. `tlsf_strerror` accepts negative values as well.

`TLSF_ERR_UID_INVALID` — `uid = 0`, except in `free_uid_blocks`.  
`TLSF_ERR_FORMAT_MISMATCH` — foreign `format_version`.

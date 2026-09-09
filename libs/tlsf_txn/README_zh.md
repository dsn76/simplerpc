# TLSF 事务型分配器（v4.1）

面向 **Linux x86_64 共享内存** 的事务型 TLSF 分配器（C17）。多个进程共用一个内存池：分配为 O(1)；锁的属主崩溃时，未完成的操作会被回滚，无需外部看门狗。

规范：`tz_tlsf_shm_v4.1.md`。池格式 `TLSF_FORMAT_VERSION = 0x00040000` —— 与 1.x / 2.x / 3.x 以及 4.0 API 草案（析构函数没有 `tlsf` / `user`）不兼容。

---

## 为什么需要它

普通 TLSF 不知道多进程，也不知道「操作做到一半被打断」。这里：

1. **共享内存中的健壮递归互斥锁（Robust recursive mutex）** —— 进程间互斥；属主死亡后，下一次 `lock` 得到 `EOWNERDEAD`。
2. **BUL** —— 每写入一个 8 字节元数据字之前，先保存旧值。未提交的栈会被回滚。
3. **属主 `uid`** —— 只有当所有属主都解除关联后，块才会归还内存池。
4. **`dc` / `data_type`** —— 有效载荷类型和「需要延迟清理」标志位于块头中，以便遍历器调用外部析构函数。

池内指针是**绝对地址**。所有进程必须以**兼容的虚拟地址**映射该区域（同一 VA；若将来改用偏移，至少基址相同——当前基址必须一致）。

---

## 构建

```bash
make            # test_tlsf_txn
make test       # 构建 + 运行
make bench      # setdc/getdc 延迟以及 malloc/free 混合
make debug      # -O0 -g -DTLSF_DEBUG
make clean
```

依赖：仅 POSIX/glibc（`pthread`、`stdatomic`）。

---

## 内存池架构

用户把一段连续缓冲区交给 `tlsf_create`。内部：

```
+------------------------------------------------------------------+
|                     SHARED MEMORY (size 字节)                    |
+----------------------------+-------------------------------------+
| Control Area = tlsf_ctrl   | Heap Area                           |
|  format_version            |  物理块链                           |
|  matrix[FLI][SLI]          |  每个：header 64 + payload ≥ 64     |
|  bitmaps                   |  块按 64 对齐                       |
|  robust recursive mutex    |                                     |
|  txn (stack_top, pid)      |                                     |
|  bul[64]                   |                                     |
|  池边界 / used_bytes       |                                     |
+----------------------------+-------------------------------------+
```

BUL 不是单独的 mmap，而是 `tlsf_ctrl` 内的数组。堆紧接在该结构之后开始，按 64 字节对齐。

### 控制结构

- **`matrix[41][16]`** —— 同一尺寸类别空闲块的双向链表头。
- **`fl_bitmap` / `sl_bitmaps[]`** —— 「该类别至少有一块」；查找合适块是对位图做 `ctz`，O(1)。
- **`mutex`** —— `PTHREAD_PROCESS_SHARED` + `ROBUST` + `RECURSIVE`。需要递归，以便 `free_uid_blocks` 的析构函数能调用 `tlsf_free` / 嵌套 `lock`。
- **`used_bytes`** —— 已分配块的*有效*字节之和（不含头）。`tlsf_free_dc` 不减少该字段：块仍处于已分配状态。

### TLSF 尺寸类别

`FLI_MAX = 41`（下标 0…40）覆盖最大 2⁴⁰ 字节的块（池最大 1 TiB）。第二级是 2 的幂内部的 16 个子类（`SLI_COUNT = 4`）。

查找：将请求上取整到 `max(align_up(size+header), 128)` → 计算 `(fl, sl)` → 取下标 ≥ 的第一个非空链表，否则取下一个 FL。

---

## 块头（64 字节）

```
偏移   字段         含义
  0    size_flags   [63:2] 大小（64 的倍数），bit0 FREE，bit1 PREV_FREE
  8    sig_dc       [63:16] magic 0xA5C3E7F1B9D2
                    [15]    dc —— 延迟清理
                    [14:0]  data_type（用户自定义，允许为 0）
 16    prev_free    仅当块在空闲链表中时有效
 24    next_free
 32    uid[12]      24 字节 = 3 个 8 字节字（BUL 整字写入）
 56    prev_size    前一个物理块的大小；用于向前合并
```

用户指针 = `header + 64`，同样按 64 对齐。

**`sig_dc` 始终以原子方式读写**（`atomic_load` / `store` / `fetch_or` / `fetch_and`）。否则无锁的 `tlsf_setdc` 会与 `malloc` 构成 C17 下的 data race。

**`tlsf_free` 不会擦除 `sig_dc`**。对 API 而言，空闲块上的 `dc` 未定义：`getdc` / `setdc` / `cleardc` / `get_data_type` 返回 `-TLSF_ERR_NOT_ALLOCATED`。下一次 `malloc` 会整字段覆盖（`dc = 0`，新的 `data_type`）。

`tlsf_txn.h` 中的 `block_header_t` 类型供析构函数使用。字段**只读**。绕过 API 改写会破坏 BUL 和完整性。

---

## 所有权模型

- 数组中的 `uid == 0` 表示空槽。通过 API 禁止 `uid = 0`，**唯一例外**是 `tlsf_free_uid_blocks`（在那里表示「查找 `dc == 1`」）。
- 同一 uid 不会在块中重复（`link` → `UID_DUPLICATE`）。
- `tlsf_free(uid)` 清空该槽。若还有其他 uid —— 块仍已分配。若数组为空 —— 物理释放 + 合并。
- `tlsf_link` 添加属主，不改变 `dc` 和 `data_type`（包括 `dc == 1` 的块）。

最多 12 个属主。

---

## 事务与 BUL

一次公开的元数据变更 ≈ 一次事务（例外：`realloc` —— 在同一次事务中 alloc+free）。

```
写字之前：  bul_push(addr, old)
写入
成功：      stack_top = 0   (commit)
崩溃 / 溢出：从栈顶向下恢复各字
```

溢出时当前事务整体回滚，errno = `BUL_OVERFLOW`。

`setdc` / `cleardc` / `getdc` / `get_data_type` **不**写入 BUL。

记录数估计（上限 64）：

| 操作 | 最大记录数 |
|------|------------|
| 带 split 的 malloc | 21 |
| free，三重合并 | 27 |
| free_dc | 1 |
| realloc（一次 txn） | 48 |
| link | 1 |

---

## 块合并

在最后一次 `free` 时：

1. 若块带 `BLOCK_PREV_FREE` —— 检查 `prev_size`（64 的倍数、位于池内、邻居空闲且签名有效）。否则**跳过**向后合并（errno 为 `BLOCK_CORRUPTED`），以免沿垃圾指针行走。
2. 若下一个物理块空闲 —— 向前合并。
3. 合并后的块插入矩阵；右侧邻居被设置 `PREV_FREE` 和 `prev_size`。

「先 prev，再 next」的顺序使三重合并发生在同一次事务中。

---

## 加锁与恢复

`lock_and_recover` / `tlsf_lock`：

1. `format_version`
2. `pthread_mutex_lock`
3. `EOWNERDEAD` → `bul_recover_internal` → `pthread_mutex_consistent`  
   新属主的递归计数为 1，不从已死进程继承。
4. `owner_pid = getpid()`

`tlsf_unlock` 不检查 `format_version`。`EPERM` → `-LOCK_FAILED`。

`tlsf_bul_recover` —— 在 `stack_top > 0` 时阻塞式手动回滚。  
`tlsf_bul_recover_nb` —— `trylock`；忙则 `-BUSY`。

---

## 延迟清理

两套不同的工具。

| | `setdc` / `cleardc` | `free_dc` |
|--|---------------------|-----------|
| 锁 | 无 | 有 |
| BUL | 无 | 有 |
| uid | 不检查 | 必需，≠ 0 |
| 多名属主 | 可以置 dc | **不**置 dc，只解除该 uid |
| used_bytes | 无 | 无 |

无锁路径：先做基本检查，再原子 RMW。检查与操作之间，另一进程可能对同一地址 free+malloc —— 标志可能落到**新**对象上。规范接受这一点。

`tlsf_getdc`：**0 或 1 是数据**，不是成功码。错误为负数。  
`tlsf_get_data_type`：**0…32767 是类型**。

---

## `tlsf_free_uid_blocks`

TLSF **不会**自己调用 `free`。它对堆做物理遍历，匹配时调用析构函数。

```c
typedef int (*tlsf_destructor_fn)(tlsf_t tlsf, void *ptr, block_header_t *block,
                                  uint16_t match_uid, void *user);
```

- `uid != 0` —— 带该属主的块。
- `uid == 0` —— 已分配且 `dc == 1` 的块；析构函数自行选择把 `uid[]` 中的哪个槽交给 `tlsf_free_nb`。
- 在互斥锁下调用。立即释放用 `tlsf_free_nb`（每个块各自 commit）。
- `>= 0` —— **从堆起点**重新遍历（合并后旧偏移已失效）。
- `< 0` —— 停止；互斥锁已经释放；原样返回该码。

若不调用 `free_nb` / `cleardc` / 不返回 `< 0`，循环将永不结束。

**批处理：** 在 `user` 中累积指针，例如返回 `-TLSF_ERR_BUSY`，在外部再 `tlsf_lock` + 一批 `free_nb`。从返回到再次 lock 之间，池不受保护。不要在析构函数内再多调一次 `tlsf_lock`：递归会解开错误的嵌套层。

应用侧推荐的批次大小是 `TLSF_FREE_BATCH`（256）。

---

## 其他操作

**`tlsf_malloc` / `calloc`** —— uid 写入 `uid[0]`，其余为零，`dc = 0`。calloc 的 `count*size` 溢出 → 不加锁直接 OOM。

**`tlsf_realloc`** —— 一次事务，一名属主。

1. 若新尺寸装进当前块（或可切下 ≥ 128 的尾巴）—— 同一指针，无拷贝；`dc`/`data_type` 保持原样。
2. 否则若**下一个**物理块空闲且合计够用 —— 吞并它（余量 split），同样无拷贝。
3. 否则 `malloc` + `memcpy` + `free`：新块继承 `data_type`，`dc = 0`。

`ptr == NULL` → 以类型 0 做 malloc；`new_size == 0` → free。不触及前一个邻居。

**`tlsf_get_block_size(tlsf, ptr)`** —— 在锁下。返回已分配块的有效大小（不含头），若块已不存在 / 指针损坏则 `0` + errno。不检查 `uid`：同一地址上的 free+malloc 看起来仍是「活」块——这是写入方的问题。

**`tlsf_free_nb`** —— 与 `free` 同一算法，但不加 `lock`。要求锁已被持有。

---

## 限制

| 参数 | 取值 |
|------|------|
| 内存池 | 8 KiB … 1 TiB |
| 对齐 | 64 字节 |
| 最小块 | 128 字节 |
| 属主数 | 12 |
| data_type | 15 位 |
| BUL | 64 × 16 字节 |
| 互斥锁 | recursive + robust，进程间共享 |

---

## 示例

```c
#include "tlsf_txn.h"
#include <sys/mman.h>

void *mem = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
tlsf_t tlsf = tlsf_create(mem, 128 * 1024);

void *buf = tlsf_malloc(tlsf, 256, /* uid */ 1, /* data_type */ 42);
tlsf_link(tlsf, buf, 2);
tlsf_free(tlsf, buf, 1);   /* 仍存活：uid 2 */
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

## 错误码

`int` API —— 负常量（`getdc` / `get_data_type` 除外）。`void` API —— `tlsf_get_errno`。`tlsf_strerror` 也接受负值。

`TLSF_ERR_UID_INVALID` —— `uid = 0`，`free_uid_blocks` 除外。  
`TLSF_ERR_FORMAT_MISMATCH` —— 外来的 `format_version`。

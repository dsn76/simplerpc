# simplerpc (libsrpc)
- 状态 — 开发中。
- 当前版本 0.3.0
- **Language:** 🇷🇺 [Русский](README.md) · 🇬🇧 [English](README_en.md) · 🇨🇳 **中文**
- [![Coverity Scan Build Status](https://scan.coverity.com/projects/33264/badge.svg)](https://scan.coverity.com/projects/simplerpc)
- [rpcgen](../../tree/rpcgen) — 用代码生成替换 X-macro 的分支。

**simplerpc** 是一个用 C 编写的库，用于同一台 Linux 机器上进程之间的远程过程调用（RPC）。在公共列表中声明的函数像普通 C 函数一样调用，实际却在定义它的那个进程中执行。

参数和结果通过共享内存传递，所有进程把这块内存映射到同一个虚拟地址。因此，指向公共池中某个块的指针在任何一个进程里都同样有效，指针背后的数据不会被拷贝。UNIX 套接字只用于事务性工作：注册、分发内存描述符、发现进程死亡。内存池由事务型 TLSF 分配器管理，进程在操作中途死亡时分配器仍然完好。

## 演示

三个终端，`example/` 中的三个程序：

```bash
./build/example/log    # 终端 1：log_write() 的执行方
./build/example/clc    # 终端 2：calc_add() 的执行方
./build/example/app    # 终端 3：调用方
```

`app` 把 `log_write(...)` 和 `calc_add(1, 2)` 当作普通函数来调用。`app` 中的字符串出现在第一个终端：是 `log` 进程把它打印出来的。加法由 `clc` 进程完成。参数序列化由 X-macro 生成，无需手工编写。用 `./build/example/all_exit` 停止各执行方。

接入自己的应用：

1. 把函数原型加进 `src/libsrpc_rpc_functions.h` 的 X-macro `RPC_LIST`。这个文件是库自身源码的一部分，不是应用单独的头文件。
2. 重新构建 `libsrpc.so`。
3. 在执行方进程中定义函数，在调用方进程中直接调用它。
4. 加上 `#include "libsrpc.h"`，并以 `-Wl,--no-as-needed` 标志把应用链接到 `libsrpc.so`。
5. 库重新构建之后，重启所有使用它的进程。

协调守护进程无需手工启动：第一个加载 `libsrpc.so` 的进程会把它拉起来。最后一个客户端断开后，守护进程自行退出。

## 📋 主要特性

- **像普通调用一样的 RPC。** 进程的角色由链接器决定：定义了函数的是执行方，没有定义的是调用方。没有 IDL，没有代码生成器，也没有运行时注册。
- **不拷贝数据的共享内存。** 内存池在所有进程中映射到同一个虚拟地址。RPC 请求只拷贝参数本身，指针背后的数据留在原地。
- **事务型 TLSF 分配器。** 分配为 O(1)。跨进程的健壮互斥锁和 BUL（Binary Undo Log）回滚日志：进程在 `malloc` 或 `free` 中途死亡时，下一个拿到互斥锁的进程回滚未完成的操作。
- **内存所有权模型。** 每个块都带有属主进程的 UID，每块最多 12 个。`libsrpc_shmem_link()` 增加属主，`libsrpc_shmem_free()` 只解除调用者自己的。块因此可以不经拷贝交给另一个进程。
- **死亡进程善后。** 守护进程从套接字断开立即得知客户端死亡，并把其名下的块归还内存池。调用方可以通过 `libsrpc_shmem_proc_lock()` 暂时保持执行方的块：在保持解除之前，这些块会熬过执行方的死亡。
- **垃圾回收器。** 守护进程中的线程释放那些其他进程可能仍在读取的服务块。占用情况通过 Hazard Pointer 检查。
- **分派。** 一次调用可由每一个已注册的执行方执行（`RPC_SEND_ALL`）、由第一个执行（`RPC_SEND_FIRST`）、由最后一个执行（`RPC_SEND_LAST`），或按轮询执行一个（`RPC_SEND_RR`）。所有执行方的应答通过 `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()` 取得。
- **队列与同步。** 每个进程有自己的无锁 MPMC 入站请求队列。执行方线程在共享内存中的 POSIX 信号量上休眠。
- **磁盘上没有守护进程文件。** 守护进程的可执行文件嵌入 `libsrpc.so`，通过 `fexecve()` 从 `memfd` 启动。
- **分配器替换（可选）。** 拦截 `malloc`/`calloc`/`realloc`/`free`，按线程切换到公共池。默认关闭，见 `DISABLE_ALLOC`。

## 🛠 要求

- **操作系统与架构：** x86_64 上的 Linux。内核须支持 `MAP_FIXED_NOREPLACE`（4.17+）。使用 `memfd_create`、`fexecve`、抽象 UNIX 套接字和 `MADV_DONTFORK`。代码中有 x86 指令（`pause`）和 `-mrdrnd` 标志。
- **语言：** C17。
- **构建：** CMake 3.10+、Make、`gcc`（内嵌加载器始终用 `gcc` 构建，与所选编译器无关）、`xxd` 工具。
- **库：** 只有系统的 `pthread` 和 `dl`。没有外部依赖，也没有 git 子模块，`tlsf_txn` 分配器就在项目树中。

## 📂 项目结构

```text
simplerpc/
├── CMakeLists.txt              # 主 CMake 配置
├── Makefile                    # CMake 包装：make all / make clean
├── doc/
│   └── ARCHITECTURE*.md        # 内部结构（ru / en / zh）
├── src/                        # libsrpc 源码
│   ├── libsrpc.h               # 公开 API
│   ├── libsrpc_rpc_functions.h # X-macro RPC_LIST —— RPC 函数列表
│   ├── libsrpc_errno.h         # libsrpc 错误码
│   ├── libsrpc.c               # 包装器、调度器、函数注册表、构造函数
│   ├── libsrpc_daemon.c        # 守护进程的启动及其主循环
│   ├── libsrpc_loader.c        # 内嵌守护进程加载器的源码
│   ├── libsrpc_unix_socket.c   # UNIX 套接字：连接、注册、传递 FD
│   ├── libsrpc_shmem.c         # 共享内存池与分配器包装
│   ├── libsrpc_shm_gc.c        # 垃圾回收器
│   ├── libsrpc_proc.c          # 进程描述符、死亡进程善后
│   ├── lf_mpmc_queue.c         # 无锁 MPMC 请求队列
│   ├── libsrpc_list_spin.c     # 单链表（写操作用 spinlock）
│   ├── libsrpc_fixblockalloc.c # 固定大小块池（守护进程的本地内存）
│   ├── libsrpc_pthread.c       # 创建线程、绑定 CPU
│   ├── libsrpc_wrapper.h       # 信号量、暂停、小型包装
│   ├── libsrpc_local.h         # 内部结构：上下文、请求、应答
│   ├── libsrpc_private.h       # RPC 标识与函数注册表
│   └── macro.h, argfunc.h      # 预处理器代码生成
├── libs/
│   └── tlsf_txn/               # 事务型 TLSF 分配器 v4.1（含测试、README）
├── example/                    # 示例
│   ├── app.c                   # 调用方
│   ├── log.c                   # log_write() 的执行方
│   ├── clc.c                   # calc_add() 的执行方，自身调用 log_write()
│   ├── all_exit.c              # 广播 all_exit()：停止各执行方
│   └── list/                   # 共享链表：所有权移交与保持
│       ├── dal_list.h          # 带健壮互斥锁的双向链表
│       ├── list_db.c           # 链表属主，list_api() 的执行方
│       └── list_cli.c          # 客户端：--add / --del / --print
└── bench/                      # 性能测试
    ├── bench_ipc_baseline.c    # 基线：pipe、socketpair
    ├── bench_srpc_server.c     # 测试用执行方
    ├── bench_srpc_client.c     # 延迟与吞吐
    └── run_bench.sh            # 测试启动脚本
```

## 🚀 构建

```bash
git clone https://github.com/dsn76/simplerpc.git
cd simplerpc
make all
```

`make all` 创建 `build` 目录，用 CMake 配置项目，并构建静态分配器库 `libtlsf.a`、`libsrpc.so`、示例和基准测试。`make clean` 删除 `build`。

手工构建：

```bash
mkdir -p build && cd build
cmake ..
make
```

### 模式与参数

模式由变量 `BUILD_TYPE` 指定：
- `release`（默认）—— `-O3`；
- `debug` —— `-O0 -ggdb3 -DDEBUG`，打开详细的 `DBG_PRINT` 输出。

| 参数 | 默认值 | 用途 |
|---|---|---|
| `BUILD_TYPE` | `release` | 构建模式：`debug` / `release` |
| `ENABLE_SANITIZER` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer（仅 `debug`） |
| `DISABLE_ALLOC` | `ON` | 不构建对 `malloc`/`calloc`/`realloc`/`free` 的替换 |
| `DBG_LVL` | `2` | 消息级别：`0` — 关，`1` — ERR，`2` — ERR+WRN，`3` — ERR+WRN+INF |
| `MPMCQ_DEGREE` | `10` | 每进程请求队列容量，以 2 的幂表示（`2^10 = 1024`） |
| `SHMEM_SIZE_KB` | `4096` | 共享内存池大小，KiB |
| `SHMEM_BASE_VADR` | `0x200000000000` | 内存池在所有进程中映射到的虚拟地址 |

```bash
cmake -DBUILD_TYPE=debug -DDBG_LVL=3 -DSHMEM_SIZE_KB=16384 ..
```

> **重要：** 每次 CMake 配置（包括每次 `make all`）都会把新的时间戳 `BUILD_TS` 写入库。守护进程名、套接字名和内存池签名都由它生成。加载了不同 `libsrpc.so` 构建的进程互相看不见：这是对 `RPC_LIST` 不一致的防护。重新构建之后必须重启所有进程。

`ERR` 和 `WRN` 消息打印到进程的 `stderr`。守护进程继承拉起它的那个进程的 `stdout`/`stderr`，因此它的消息出现在那个进程的终端里。

## 💡 使用

### 声明 RPC 函数

所有可远程调用的函数都列在同一个 X-macro `src/libsrpc_rpc_functions.h` 中。目前里面是示例函数：

```c
#define RPC_LIST    \
    XF(RPC_SEND_ALL, int,    testlocal) \
    XF(RPC_SEND_ALL, pid_t,  log_write, const char*) \
    XF(RPC_SEND_ALL, int,    calc_add, int, int) \
    XF(RPC_SEND_ALL, void,   all_exit) \
    XF(RPC_SEND_ALL, void *, list_api, int, void *) \
```

条目格式：`XF(分派策略, 返回类型, 名字, 参数类型...)`。

- **策略。** `RPC_SEND_ALL` —— 发给所有执行方。`RPC_SEND_FIRST` —— 发给注册表中第一个可用的。`RPC_SEND_LAST` —— 发给注册表中最后一个。`RPC_SEND_RR` —— 按轮询发给一个。
- **类型。** 参数和结果按字节拷贝，因此允许标量类型和指针。指针只有指向共享池时才有意义。不支持变参函数。
- **类型可见性。** 类型必须在 `RPC_LIST` 之前声明：`libsrpc.h` 包含 `<stdint.h>` 和 `<sys/types.h>`，自己的类型要从 `libsrpc_rpc_functions.h` 包含进来。
- **标识。** 每个函数生成一个 `libsrpc_funid_<名字>`，供 `libsrpc_timeout_func_set()` 和 `libsrpc_fnreg_num_get()` 使用。

### 谁是执行方，谁是调用方

没有单独的服务端 API。**定义了** `RPC_LIST` 中函数的进程成为它的**执行方**：应用的强符号覆盖 `libsrpc.so` 中的弱桩符号。在该进程内部调用这个函数就是普通的本地调用。没有定义函数的进程在调用时进入桩，由桩发出 RPC 请求。角色按函数分别决定：一个进程可以执行某些函数，同时调用另一些。

```c
/* 执行方：定义函数。 */
pid_t log_write(const char *msg) {
    fprintf(stderr, "LOG: '%s'\n", msg);
    return getpid();
}
```

```c
/* 调用方：直接调用。 */
char *str = libsrpc_shmem_malloc(1024);          /* 字符串必须位于内存池中 */
snprintf(str, 1024, "Hello from pid=%d", getpid());

pid_t pid = log_write(str);                      /* 第一个执行方的结果 */
if (libsrpc_errno_get() != 0) { /* 调用没有发出 */ }

/* 所有执行方的结果（RPC_SEND_ALL）： */
for (int i = 0, num = libsrpc_lastreq_num(); i < num; i++) {
    int rc = libsrpc_lastreq_get(i, &pid, sizeof(pid));
    if (rc == 0) printf("pid[%d]=%d\n", i, pid);
    else         printf("slot %d: %s\n", i, libsrpc_strerror(-rc));
}
libsrpc_shmem_free(str);
```

执行方线程自己不能发起 RPC 调用：请求不会发出，`libsrpc_errno_get()` 返回非零码（`ERECURSIVE`）。执行方进程的其他线程，包括主线程，照常发起 RPC。

### 调用结果与错误

生成的包装器返回第一个应答槽的结果。没有应答时返回该类型的零值（`0`、`NULL`）。零结果本身不能区分错误与成功，因此调用状态用两种方式检查：

- `libsrpc_errno_get()` —— 调用**没有发出**的原因：`ENOREGFUN`（没有执行方）、`ERPCDISABLE`（与守护进程失去联系）、`ERECURSIVE`、`ENOTAVAILABLE`（没有执行方接受请求）、`ENOMEM`、`ESHMNOINIT`。码是正数，包装器在每次调用开始时清零。
- `libsrpc_lastreq_get(i, ...)` —— **已发出**请求在槽 `i` 中的结局：`0` —— 收到应答，`-ERPCWAITIMEDOUT` —— 执行方没有及时应答，`-ECANCELLED` —— 执行方在取走请求之前断开，`-ENOMEM` —— 执行方队列已满。超时和取消不会出现在 `libsrpc_errno_get()` 中。

当 `libsrpc_errno_get() == 0` 且所需槽返回 `0` 时，调用才算成功。两个条件都要检查。如果调用没有发出，槽里可能仍留着同一线程**上一次**调用的应答。对有返回值的函数，未发出的原因有时会被替换成 `ETIMEDOUT`，但非零码仍然是失败的可靠标志。

大于 4095 的码是 libsrpc 自己的码（列表在 `src/libsrpc_errno.h`），其余是系统 `errno`。`libsrpc_strerror(code)` 返回码对应的字符串。

### 参数所用的内存

凡是通过指针传递的东西都必须位于内存池中：`libsrpc_shmem_malloc()`、`libsrpc_shmem_calloc()`、`libsrpc_shmem_realloc()`。块属于分配它的进程。`libsrpc_shmem_free()` 只解除当前进程的属主身份，当属主全部消失时块归还内存池。进程退出时没有释放的块由守护进程释放。

要让块比它的创建者活得更久，接收方调用 `libsrpc_shmem_link(ptr)` 成为第二个属主，之后创建者就可以调用 `free`。所有权就这样不经拷贝转移到另一个进程：`list` 示例演示了这个做法。

内存池大小在构建时固定（`SHMEM_SIZE_KB`，默认 4 MiB），运行中不会增长。

### 超时

等待应答受微秒级超时限制：

- `libsrpc_timeout_oneshot_set(us)` —— 当前线程的下一次调用；
- `libsrpc_timeout_func_set(libsrpc_funid_<名字>, us)` —— 某个函数，限于当前线程；
- `libsrpc_timeout_global_set(us)` —— 进程所有线程的所有调用。

优先级按列表顺序。默认超时 1 秒，不能设得小于 100 微秒。每收到一个应答，间隔就重新计时，因此分派给多个执行方时，总等待时间可能超过设定值。

### 链接

一个只执行函数、自己不调用 `libsrpc.so` 中任何符号的进程，必须用 `-Wl,--no-as-needed` 链接。否则链接器会丢掉对库的依赖，库的构造函数不会执行，进程也就不会接入守护进程。示例和基准测试始终带这个标志链接。

库的构造函数在进入 `main()` 之前等待与守护进程的连接。`main()` 开始时，进程已经注册完毕并在接收请求。

## 📖 公开 API

所有声明都在 `src/libsrpc.h` 中。最近一次调用的结果、错误码、一次性超时、按函数的超时以及保持，都属于当前线程。

**内存池**

- `void *libsrpc_shmem_malloc(size_t size)`、`void *libsrpc_shmem_calloc(size_t num, size_t size)` —— 分配块，属主是当前进程。出错返回 `NULL`。
- `void *libsrpc_shmem_realloc(void *ptr, size_t newsize)` —— 改变块的大小。只有当前进程是该块的唯一属主时才有效，否则返回 `NULL`。
- `void libsrpc_shmem_free(void *ptr)` —— 解除当前进程的所有权。块随最后一个属主一起归还内存池。
- `int libsrpc_shmem_link(void *ptr)` —— 把当前进程加为属主（不超过 12 个）。返回 `0` 或负码。
- `int libsrpc_shmem_get_size(void *ptr, size_t *psz)` —— 把块的有效载荷大小写入 `*psz`。成功返回 `0`。

内存函数用分配器码 `TLSF_ERR_*`（1…15，`libs/tlsf_txn/tlsf_txn.h`）报告错误，同样的码也会进入 `libsrpc_errno_get()`。这些编号与系统 `errno` 重叠，因此要用 `tlsf_strerror()` 解读，而不是 `libsrpc_strerror()`。

**最近一次调用的结果**

- `int libsrpc_lastreq_num(void)` —— 当前线程最近一次 RPC 调用的应答槽数量（没有调用过则为 `0`）。
- `int libsrpc_lastreq_get(int idx, void *retval, size_t sz)` —— 把槽 `idx` 的结果拷贝到 `retval`。`sz` 必须与返回类型的大小完全一致，`void` 为 `0`，否则返回 `-EBADMSG`。返回 `0` 或槽的负码。

**保持执行方的内存**

- `int libsrpc_shmem_proc_lock(int idx, void *ptr)` —— 保持最近一次调用中槽 `idx` 对应执行方的 UID，前提是块 `ptr` 属于该执行方。保持有效期间，执行方死亡不会把它的用户块归还内存池。返回保持槽号（`0…3`）或负码：`-ENOTFOUND`（块不属于它）、`-EPROCDESTROYED`（执行方已经断开）、`-ENOMEM`（四个槽都已占用）、`-EINVALREQUEST` / `-EINVALRESPONSE` / `-ENOTAVAILABLE`（没有合适的调用或槽）。
- `int libsrpc_shmem_proc_unlock(int slot)` —— 解除一个保持。`int libsrpc_shmem_proc_all_unlock(void)` —— 解除该线程的全部保持。

只有取走保持的那个线程才能解除它。该线程之后的 RPC 调用不会清掉保持。持有者线程或进程结束时，保持自行消失。

**注册表与超时**

- `int libsrpc_fnreg_num_get(libsrpc_funid_t funid)` —— 函数已注册的执行方数量；`funid` 无效时返回 `-EINVAL`。
- `libsrpc_timeout_oneshot_set()`、`libsrpc_timeout_func_set()`、`libsrpc_timeout_global_set()` —— 见「超时」。

**错误**

- `int libsrpc_errno_get(void)` —— 当前线程的最近一次错误码。
- `const char *libsrpc_strerror(int code)` —— 正数码的字符串，可以是系统码或 libsrpc 自己的码。

**分配器替换**（仅在以 `-DDISABLE_ALLOC=OFF` 构建时）

- `libsrpc_alloc_sw_shm()` / `libsrpc_alloc_sw_std()` —— 把当前线程的 `malloc`/`calloc`/`realloc`/`free` 切换到内存池，以及切回。

## ▶️ 示例

所有示例构建到 `build/example/`。执行方一直运行，直到收到 `all_exit()`。

| 程序 | 角色 |
|---|---|
| `log` | `log_write()` 和 `all_exit()` 的执行方 |
| `clc` | `calc_add()` 和 `all_exit()` 的执行方；自身调用 `log_write()` |
| `app` | 调用方：`log_write()` 和 `calc_add()`。自己的 `testlocal()` 本地调用 |
| `all_exit` | 广播 `all_exit()`：停止 `log`、`clc` 和 `list_db` |
| `list_db` | 持有共享链表，`list_api()` 和 `all_exit()` 的执行方 |
| `list_cli` | 链表客户端：`--add`、`--del`、`--print` |

```bash
./build/example/log         # 终端 1
./build/example/clc         # 终端 2
./build/example/app         # 终端 3
./build/example/all_exit    # 停止
```

`app` 在内存池中分配字符串并调用 `log_write()`：字符串打印在 `log` 的终端里。随后 `calc_add(1, 2)` 在 `clc` 进程中计算。如果 `clc` 没有运行，调用返回 `0`，`libsrpc_errno_get()` 返回 `ENOREGFUN`。

### list 示例：共享链表与所有权移交

`example/list/` 中的示例用一个存放在内存池里、多个进程都能访问的字符串双向链表来展示内存所有权模型。

**角色。** `list_db` 定义 `list_api(int op, void *node)` 并成为它的执行方。`list_cli` 不定义这个函数，因此它的调用经 RPC 到达 `list_db`。操作：
- `DAL_LIST_OP_GET_HEAD` —— 返回链表头指针；
- `DAL_LIST_OP_INSERT` —— 插入节点；
- `DAL_LIST_OP_REMOVE` —— 删除节点。

**`list_db` 的启动。** 通过 `libsrpc_fnreg_num_get(libsrpc_funid_list_api)` 检查是否已有另一个 `list_api()` 执行方。如果有，第二个实例退出：示例中只有一个链表。然后在内存池中分配链表头，并在其中初始化带 `PTHREAD_PROCESS_SHARED` 的健壮互斥锁。进程通过这个互斥锁约定谁正在修改或读取链表链接。链表头属于 `list_db`，因为是它分配的。

**添加（`--add`）—— 移交所有权。**

```c
list_cli_node_t *node = libsrpc_shmem_malloc(sizeof(*node) + len + 1);
/* 填入 size 和 data */
void *ptr = list_api(DAL_LIST_OP_INSERT, node);
libsrpc_shmem_free(node);
```

`list_cli` 分配节点并把指针传给 `list_api()`。在 `list_db` 中，插入从 `libsrpc_shmem_link(node)` 开始：节点有了两个属主。然后节点在互斥锁保护下插入链表。调用返回后，`list_cli` 调用 `libsrpc_shmem_free(node)`。这只解除它自己的 UID，节点留在链表中，现在只属于 `list_db`。`list_cli` 退出不再影响链表。

**打印（`--print`）—— 保持执行方的内存。** `list_cli` 用 `list_api(DAL_LIST_OP_GET_HEAD, NULL)` 取得链表头，并立即调用 `libsrpc_shmem_proc_lock(0, list_head)`。库检查链表头属于槽 0 的执行方，并保持它的 UID。保持有效期间，`list_db` 突然死亡既不会把链表头、也不会把交给它的节点归还内存池。随后 `list_cli` 取得互斥锁，遍历链表，打印字符串，放开互斥锁，并用 `libsrpc_shmem_proc_unlock()` 解除保持。

**删除（`--del`）。** 按同样的顺序查找字符串：链表头、保持、互斥锁、遍历。找到的节点传给 `list_api(DAL_LIST_OP_REMOVE, node)`。在 `list_db` 中节点被摘下并用 `libsrpc_shmem_free()` 释放。此时 `list_db` 是唯一属主，块因此归还内存池。

```bash
./build/example/list_db                                   # 终端 1
./build/example/list_cli --add hello --add world --print  # 终端 2
size: 30, data: world
size: 30, data: hello
./build/example/list_cli --del hello --print
size: 30, data: world
./build/example/all_exit
```

节点插入链表头部，因此按相反顺序打印。`size` 是整个节点连同头部的大小。这是一个教学示例：其中没有实现链表互斥锁在 `EOWNERDEAD` 之后的恢复。

## 📈 性能测试

```bash
make all
./bench/run_bench.sh build 4000 4    # 构建目录、迭代次数、线程数
```

脚本拉起执行方，跑基线（`pipe`、`socketpair`），然后跑单线程和多线程的 RPC 往返。打印延迟分布（min/p50/p90/p99/max）和吞吐。

## ⚠️ 限制

- 仅限 Linux x86_64，且仅限同一台机器上的进程。
- 所有进程必须加载同一份 `libsrpc.so` 构建。`RPC_LIST` 编译进库中，因此修改函数列表需要重新构建库并重启所有进程。
- 守护进程是单一故障点。它一旦死亡，已经在运行的进程中的 RPC 会被关闭，并且不会自行恢复。新进程会拉起一个带新内存池的新守护进程，看不到旧进程。
- 不检查客户端权限。任何连上守护进程抽象套接字的进程都能访问整个内存池。库不适用于互不信任的进程。
- 每个加载了库的进程，哪怕只是调用方，都会按 CPU 数量启动执行方线程。每次请求之后，它们在入睡前会短暂地忙等。
- 超时从最近一次到达的应答起算，而不是从调用开始起算。
- 内存池大小在构建时固定。

## 📚 文档

- [doc/ARCHITECTURE_zh.md](doc/ARCHITECTURE_zh.md) —— 内部结构、调用生命周期、故障与恢复。
- [libs/tlsf_txn/README_zh.md](libs/tlsf_txn/README_zh.md) —— 事务型 TLSF 分配器。

## 📜 许可证

`libsrpc` 代码以 **Apache-2.0** 许可证分发。详见 [LICENSE](LICENSE)。

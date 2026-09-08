# simplerpc (libsrpc)
- 状态 — 开发中。
- 当前版本 0.2.1
- **Language:** 🇷🇺 [Русский](README.md) · 🇬🇧 [English](README_en.md) · 🇨🇳 **中文**
- [![Coverity Scan Build Status](https://scan.coverity.com/projects/33264/badge.svg)](https://scan.coverity.com/projects/simplerpc)

**simplerpc** 是一个轻量级、高性能的 Linux 进程间远程过程调用（RPC）框架，使用 C 语言编写。

本项目提供基于共享内存（Shared Memory）和 UNIX socket 的透明进程间通信（IPC）机制，并自带无锁（lock-free）原语（MPMC 队列、函数注册表）以及一个事务型共享内存分配器。

## 演示
终端一：`./log`（可启动多个实例）。终端二：`./clc`（同上）。终端三：`./app`。第一个终端的输出中会出现 `LOG("Hello, world, from app! my pid=...")`。`app` 中的调用 `log_write("Hello")` 和 `calc_add(1, 2)` 是普通的 C 函数，但实际执行发生在 `log` 和 `clc` 进程中。函数参数的全部序列化都隐藏在 X-macro 中，无需手工编写。要在自己的应用中使用：在源码中 `#include "libsrpc.h"`，在 `./src/libsrpc_rpc_functions.h` 中声明要导出的函数原型（它是库自身源码的一部分，并非独立的公开 API 头文件），重新构建 `libsrpc.so`，并以 `-Wl,--no-as-needed` 标志链接到你的应用中。更多细节见 `./example`。

## 项目简介
**simplerpc** 项目（`libsrpc` 库）是一个面向 Linux 环境、用于在不同进程间组织远程过程调用（RPC）的高性能框架。
其架构基于：使用共享内存进行数据传输，使用 UNIX socket 进行信令交互和分发内存描述符。共享段中的内存管理由事务型 **TLSF** 分配器（`tlsf_txn` v4.1）完成，其源码包含在项目树中（`libs/tlsf_txn/`）。线程与进程间的同步建立在放置于共享内存（`pshared`）中的 POSIX 信号量之上，以及无锁结构之上：MPMC 队列（Multi-Producer Multi-Consumer）和基于 CAS 发布的函数注册表。

本项目的特色之一是**没有 IDL、没有代码生成器**：进程的角色（调用方或执行方）在链接期通过 `__attribute__((weak, alias))` 确定，全部参数序列化由单个 X-macro `RPC_LIST` 经预处理器生成。第二个特色是内嵌的静态加载器（embedded loader）：它经 `xxd` 工具编译为 C 数组并集成进 `libsrpc.so` 动态库，使库能够直接从内存（而非磁盘文件）拉起后台协调进程（守护进程）。


## 📋 主要特性

- **进程间 RPC：** 透明地从一个进程的地址空间调用在另一个进程中实现的函数。
- **共享内存（Shared Memory）：** 所有进程以同一虚拟地址映射的公共内存区域，用于无冗余拷贝的数据交换。
- **事务型 TLSF 分配器：** `tlsf_txn` 分配器，具备确定性 O(1) 分配、健壮互斥锁（robust mutex）和 BUL（Binary Undo Log）回滚日志：进程在操作中途崩溃不会破坏内存池。
- **内存所有权模型：** 内存池中的每个块都带有属主进程的 UID 标记（每块最多 12 个属主）。进程断开时，守护进程自动释放其名下所有块。
- **垃圾回收器：** 守护进程中的后台 GC 线程回收被标记为延迟清理的服务块，并通过 Hazard Pointer 检查其占用情况。
- **RPC 函数的动态链接：** 进程向守护进程发送自己能够执行的函数的位图；守护进程将其注册进共享内存中的无锁注册表。
- **多播：** 一次调用可由所有已注册进程执行（`RPC_SEND_ALL`），或仅由第一个进程执行（`RPC_SEND_FIRST`）；结果通过 `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()` 收集。
- **同步与队列：** 每进程一条无锁 MPMC 队列，以及共享内存中的 POSIX 信号量，用于执行方线程的休眠/唤醒。
- **嵌入式加载器（Embedded Loader）：** 库内部静态编译的加载器，通过 `fexecve()` 从 `memfd` 启动守护进程——磁盘上无文件。
- **可选的分配器替换：** 拦截 `malloc`/`calloc`/`realloc`/`free`，可"热切换"到共享内存池（默认关闭，见 `DISABLE_ALLOC`）。

## 🛠 技术栈

- **编程语言：** C17
- **构建系统：** CMake（版本 3.10+）、Make
- **操作系统：** Linux（x86_64；使用 `memfd_create`、`fexecve`、抽象 UNIX socket、`MAP_FIXED_NOREPLACE`、`MADV_DONTFORK`）
- **依赖：**
  - 标准库：`pthread`（线程、信号量）、`dl`（动态加载）。
  - `xxd` 工具——构建期使用，用于内嵌加载器。
  - 无外部依赖、无 git 子模块：`tlsf_txn` 分配器包含在项目树中。

## 📂 项目结构

```text
simplerpc/
├── CMakeLists.txt              # CMake 主配置文件
├── Makefile                    # 辅助 Makefile（CMake 的封装）
├── doc/
│   ├── ARCHITECTURE.md         # 内部结构说明（俄文）
│   ├── ARCHITECTURE_en.md      # 内部结构说明（英文）
│   └── ARCHITECTURE_zh.md      # 内部结构说明（中文）
├── src/                        # libsrpc 库核心源码
│   ├── libsrpc.h               # 公开 API
│   ├── libsrpc_rpc_functions.h # RPC_LIST X-macro —— RPC 函数列表
│   ├── libsrpc.c               # 包装器代码生成、调度器、函数注册表
│   ├── libsrpc_daemon.c        # 守护进程启动（spawn）及其主循环
│   ├── libsrpc_loader.c        # 内嵌加载器源码
│   ├── libsrpc_unix_socket.c   # 基于 UNIX socket 的传输层
│   ├── libsrpc_shmem.c         # 共享内存与分配器管理
│   ├── libsrpc_shm_gc.c        # 共享内存垃圾回收器
│   ├── libsrpc_proc.c          # 共享内存中的进程描述符
│   ├── libsrpc_mpmcq.c         # 无锁 MPMC 请求队列
│   ├── libsrpc_list_spin.c     # 单链表（写操作用自旋锁）
│   ├── libsrpc_fixblockalloc.c # 定长块池（本地内存）
│   ├── libsrpc_pthread.c       # 线程创建封装、CPU 绑定
│   ├── libsrpc_errno.c         # libsrpc 错误码与错误字符串
│   ├── libsrpc_local.h         # 内部结构：上下文、请求、应答
│   ├── libsrpc_private.h       # RPC 标识符与函数注册表
│   └── macro.h, argfunc.h      # 预处理器代码生成
├── libs/                       # 第三方源码（树内，无子模块）
│   └── tlsf_txn/               # 事务型 TLSF 分配器 v4.1（+ 测试、README）
├── example/                    # 演示应用
│   ├── app.c                   # 调用方进程（caller）
│   ├── log.c                   # log_write() 的执行方
│   ├── clc.c                   # calc_add() 的执行方，其自身通过 RPC 调用 log_write()
│   └── all_exit.c              # 广播停止所有执行方
└── bench/                      # 性能测试
    ├── bench_ipc_baseline.c    # 基线：pipe、socketpair
    ├── bench_srpc_server.c     # 测量用的执行方进程
    ├── bench_srpc_client.c     # 延迟与吞吐量测量
    └── run_bench.sh            # 测量启动脚本
```

## 🚀 构建与安装

项目没有子模块，普通克隆即可。

### 1. 克隆仓库
```bash
git clone https://github.com/dsn76/simplerpc.git
cd simplerpc
```

### 2. 构建项目
项目附带 `Makefile`，自动化 CMake 的调用。

```bash
make all
```
*该命令创建 `build` 目录，用 CMake 配置项目，并编译静态分配器库 `libtlsf.a`、动态库 `libsrpc.so`、示例程序和性能测试。*

手动使用 CMake 构建：
```bash
mkdir -p build && cd build
cmake ..
make
```

清理：`make clean`（删除 `build` 目录）。

### 构建模式与参数

构建模式由 `BUILD_TYPE` 变量设定：
- `release`（默认）：`-O3` 优化。
- `debug`：调试信息 `-O0 -ggdb3 -DDEBUG`（启用详细的 `DBG_PRINT` 输出）。

另外支持以下 CMake 参数：

| 参数 | 默认值 | 用途 |
|---|---|---|
| `BUILD_TYPE` | `release` | 构建模式：`debug` / `release` |
| `ENABLE_SANITIZER` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer（仅限 `debug`） |
| `DISABLE_ALLOC` | `ON` | 禁用将标准 `malloc`/`calloc`/`realloc`/`free` 替换为共享内存分配器 |
| `DBG_LVL` | `2` | 消息级别：`0` —— 关闭，`1` —— ERR，`2` —— ERR+WRN，`3` —— ERR+WRN+INF |
| `MPMCQ_DEGREE` | `10` | MPMC 队列容量，以 2 的幂表示（`2^10 = 1024`） |
| `SHMEM_SIZE_KB` | `4096` | 共享内存大小（千字节） |
| `SHMEM_BASE_VADR` | `0x200000000000` | 共享内存映射的基虚拟地址 |

示例：
```bash
cmake -DBUILD_TYPE=debug -DDBG_LVL=3 -DSHMEM_SIZE_KB=16384 ..
```

> **重要：** 每次 CMake 配置都会生成 `BUILD_TS` 时间戳，它构成守护进程名、抽象 socket 名和共享内存签名。使用不同构建的 `libsrpc.so` 编译出的应用**彼此无法互通**——这是防止 `RPC_LIST` 版本不兼容的保护措施。重新构建库之后，必须重启所有参与进程。

## 💡 使用示例

### 声明 RPC 函数

所有可供远程调用的函数都列在一个 X-macro 中：`src/libsrpc_rpc_functions.h`：

```c
#define RPC_LIST    \
    XF(RPC_SEND_ALL, int,   testlocal) \
    XF(RPC_SEND_ALL, pid_t, log_write, const char*) \
    XF(RPC_SEND_ALL, int,   calc_add, int, int) \
    XF(RPC_SEND_ALL, void,  all_exit) \
```

格式：`XF(分派策略, 返回类型, 名称, 参数类型...)`。不允许可变参数函数。

### 谁是执行方，谁是调用方

没有单独的"服务端" API。在自己的代码中**定义**了 `RPC_LIST` 中某个函数的进程，自动成为该函数的**执行方**——强符号覆盖 `libsrpc.so` 中的弱 alias。未定义该函数的进程，调用时得到的是桩函数，会发出 RPC 请求：

```c
/* 执行方进程：定义了函数——即成为它的服务器。 */
pid_t log_write(const char* msg) {
    fprintf(stderr, "LOG: '%s'\n", msg);
    return getpid();
}
```

```c
/* 调用方进程：直接调用，什么也不定义。 */
char *str = libsrpc_shmem_malloc(1024);
snprintf(str, 1024, "Hello from pid=%d", getpid());

pid_t pid = log_write(str);          /* 经 RPC 发给所有执行方 */

/* 收集来自所有执行方的结果（RPC_SEND_ALL）： */
for (int i = 0, num = libsrpc_lastreq_num(); i < num; i++) {
    int rc = libsrpc_lastreq_get(i, &pid, sizeof(pid));
    if (!rc) printf("pid[%d]=%d\n", i, pid);
}
libsrpc_shmem_free(str);
```

> 传给 RPC 函数的指针必须指向共享池（`libsrpc_shmem_malloc`）——库只拷贝参数本身，不拷贝指针所指向的数据。

> 链接一个**仅**导出 RPC 函数的应用时，必须加 `-Wl,--no-as-needed` 标志：若没有对 `libsrpc.so` 符号的引用，链接器会丢弃 `DT_NEEDED`，库的构造函数便不会执行。

### 运行示例

`example/` 目录包含四个应用。无需手动启动后台协调守护进程——它由第一个链接 `libsrpc.so` 的进程自动拉起，并在最后一个客户端断开后退出。

1. **`log`** —— 执行方进程，提供 `log_write()`。
2. **`clc`** —— `calc_add()` 的执行方进程，其自身通过 RPC 调用 `log_write()`。
3. **`app`** —— 调用方进程：在共享池中分配字符串，调用 `log_write()` 和 `calc_add()`，打印结果。
4. **`all_exit`** —— 调用 `all_exit()`，广播停止所有执行方。

**启动执行方（在各自的终端中）：**
```bash
./build/example/log
./build/example/clc
```

**启动客户端：**
```bash
./build/example/app
```

**停止执行方：**
```bash
./build/example/all_exit
```

运行过程中，客户端在共享池中分配内存（`libsrpc_shmem_malloc`），写入字符串，并发起对日志函数的 RPC 调用——该调用在 `log` 进程中执行。`calc_add()` 函数则在 `clc` 进程中执行。

## 📈 性能测试

`bench/` 目录包含 RPC 调用延迟的测量，并与基础 IPC 机制对比：

```bash
make all
./bench/run_bench.sh build 4000
```

脚本会拉起执行方进程，运行基线测试（`pipe`、`socketpair`），然后进行单线程和多线程的 round-trip RPC 测试，并打印延迟分布（min/p50/p90/p99/max）和吞吐量。

## 📚 文档

- [doc/ARCHITECTURE_zh.md](doc/ARCHITECTURE_zh.md) —— 内部结构、组件、RPC 调用的生命周期。
- [libs/tlsf_txn/README_zh.md](libs/tlsf_txn/README_zh.md) —— 事务型 TLSF 分配器的文档。

## 📜 许可证

`libsrpc` 代码以 **Apache-2.0** 许可证发布。详情见 [LICENSE](LICENSE) 文件。

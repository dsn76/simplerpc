# SimpleRPC 架构

本文档描述 `libsrpc` 的内部结构：库由哪些部分组成，一次调用如何进行，进程死亡时发生什么，以及当前实现的薄弱之处。面向使用者的一面——构建、API、示例——见 [README](../README_zh.md)。

## 🏗 总体理念

**simplerpc** 是建立在共享内存之上的 RPC。传统 RPC 系统把调用序列化成字节流（Protobuf、JSON）再经套接字送出。`simplerpc` 把参数放进公共内存池，所有进程把这个池映射到同一个虚拟地址。在一个进程中取得的指针在另一个进程中同样有效，套接字只用于事务性消息。

### 核心原则

1. **零配置守护进程（Zero-Conf Daemon）。** 第一个进程加载库时，后台协调进程自行启动，并在最后一个客户端断开后退出。没有配置，没有 PID 文件，也没有服务。
2. **内存为中心（Memory-Centric）。** 进程描述符、函数注册表、队列、请求和应答都位于事务型 TLSF 分配器管理的共享池中。调用数据不经过套接字。
3. **链接期分派（Link-Time Dispatch）。** 链接器通过 `weak alias` 为每个函数决定进程的角色：定义了函数就是执行方，没有定义就是调用方。没有 IDL，也没有代码生成器，一切由单个 X-macro 经预处理器展开。
4. **基于所有权的清理（Ownership-Based Cleanup）。** 内存池中的每个块都带有属主进程的 UID。按这个 UID 为死亡进程善后，无需单独的分配登记簿。存活的请求可以保持某个进程的 UID，从而推迟其用户块的清理。
5. **透明 IPC（可选）。** 替换 `malloc`/`free` 可使现有代码无需修改即可使用内存池。默认关闭（`DISABLE_ALLOC=ON`）。

## 🧩 组件

### 1. 协调守护进程（Guard Daemon）

守护进程拥有内存池、函数注册表和进程描述符。RPC 流量不经过它：守护进程只分发内存、注册执行方，并为死亡者善后。

* **启动。** `spawn_daemon()` 做两次 `fork()`，中间夹一次 `setsid()`。第一个子进程立即退出，父进程用 `waitpid()` 回收它。第二个子进程成为没有控制终端的孤儿，也不留下僵尸。
* **内嵌加载器。** 守护进程的可执行文件不在磁盘上。构建时 `src/libsrpc_loader.c` 由 `gcc` 编译成一个没有符号的小型动态链接 ELF。`xxd -i -n loader` 把它变成 C 数组，数组再编译进 `libsrpc.so`。启动时数组写入 `memfd`，经 `fexecve()` 执行。`argv[0]` 传入守护进程名 `"simplerpc_" BUILD_TS`。环境只有一个变量 `LIBSIMPLERPC_SO`，其值是经 `dladdr()` 取得的库路径。加载器 `dlopen()` 这个库，并把控制权交给 `simplerpc_daemon_main()`。
* **为什么是进程而不是线程。** 守护进程需要干净的地址空间：它把内存池映射到固定地址 `SHMEM_BASE_VADR`，不能背上拉起它的那个应用的分配、线程和描述符。它的寿命也不应取决于那个应用的寿命。
* **选出领导者。** 每个进程都尝试拉起守护进程，但抽象套接字的 `bind()` 是原子的。失败者得到 `EADDRINUSE` 并静默退出。客户端在 `ECONNREFUSED` 时重试 `connect()`，最多 10 000 次，每次暂停 100 微秒，合计约一秒。
* **职责。** 监听抽象套接字，创建内存池并分发其描述符，维护函数注册表，启动垃圾回收线程，为断开的客户端善后。
* **退出。** 主循环是超时 1 秒的 `epoll_wait()`。客户端计数降到零时守护进程退出，内存池的匿名 `memfd` 随最后一次引用消失。
* **输出。** 守护进程继承拉起它的那个进程的 `stdout` 和 `stderr`，因此它的消息出现在那个进程的终端里。

### 2. 传输（UNIX 套接字与 SCM_RIGHTS）

套接字只用于连接、注册函数、传递内存池描述符，以及发现客户端死亡。

* **抽象套接字。** 名字是 `"simplerpc_" BUILD_TS`，其中 `BUILD_TS` 是构建配置的时间戳。加载了不同 `libsrpc.so` 构建的进程落入互不相交的域。抽象套接字没有文件系统权限。
* **连接顺序**（`libsrpc_unix_server_addclient`）：`accept4()` → `SO_PEERCRED`（凭证中只取 PID）→ `libsrpc_proc_create(pid)` → `SO_RCVTIMEO = 5 秒` → 读取注册消息 → 注册函数 → 传递内存池 FD → 加入 `epoll`。签名不对的客户端进不了内存池。不检查客户端的 UID。
* **映射内存池。** 收到 FD 后，客户端以 `MAP_FIXED_NOREPLACE` 在 `SHMEM_BASE_VADR` 上 `mmap()`，并核对内存池头部的签名。地址已被占用时，`mmap()` 失败，而不是覆盖别人的映射。
* **死亡检测。** 连接断开（`EPOLLHUP | EPOLLRDHUP` 或 `recv() == 0`）是客户端已不存在的唯一且充分的标志。套接字由内核关闭，因此 `exit()`、`SIGKILL` 和段错误都会同样带来这个事件。为此不需要心跳，也不需要超时。
* **客户端控制线程。** 应用中由一个 detached 的 `unix_client` 线程维护连接。它连接、发送注册、接收 FD、映射内存池、找到自己的进程描述符、启动执行方线程、把状态置为 `RUN` 并允许 RPC（`rpc_enable`）。库的构造函数在信号量上等待，直到该线程完成初始化或报告错误，因此进入 `main()` 时进程已经就绪。随后线程在 `epoll_wait()` 中休眠。退出时通过 `eventfd` 唤醒它。

### 3. 内存管理（共享内存中的 TLSF）

* **tlsf_txn v4.1。** 事务型 TLSF 分配器，源码在项目树中（`libs/tlsf_txn/`）。依靠 `FL × SL` 空闲链表矩阵和位图，分配为 O(1)。内存池格式带版本号（`TLSF_FORMAT_VERSION = 0x00040000`），外来格式会被拒绝。
* **池内的健壮互斥锁。** 跨进程互斥由 `pthread_mutex_t` 提供，属性为 `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST | PTHREAD_MUTEX_RECURSIVE`，放在内存池的控制结构里。`libsrpc` 自己不拿这把锁，只调用 `tlsf_*`。例外是垃圾回收器：它在一次显式的 `tlsf_lock()` 之下成批释放块。
* **BUL（Binary Undo Log）。** 每写一个 8 字节的元数据字之前，旧值被压入池内的回滚栈，操作成功则重置栈（提交）。若互斥锁属主死亡，下一次加锁收到 `EOWNERDEAD`，回滚未完成的操作并调用 `pthread_mutex_consistent()`。无需外部看门狗，内存池保持一致。回滚的只是分配器的元数据：日志不保护块内的数据。
* **属主（UID）。** 块头有数组 `uid[12]`。只有当所有属主都从数组中划掉之后，块才归还内存池。`libsrpc` 向分配器传递 `proc_uid`——守护进程发给每个客户端的 16 位标识（守护进程自身为 `DAEMON_PROC_UID = 1`）。进程断开后标识会被复用。
  * `libsrpc_shmem_link()` 增加属主。
  * `libsrpc_shmem_free()` 只划掉调用者的 UID。
  * `libsrpc_shmem_realloc()` 只对唯一属主的块有效。
  * `libsrpc_shmem_get_size()` 读取有效载荷大小。
* **块的类型。** 头部中 15 位的 `data_type` 字段存放 `libsrpc_shm_data_type_t`：`USER`（应用数据）、`PROC`（进程描述符）、`REGFN`（函数注册表块）、`REQUEST`（请求块）。析构函数和垃圾回收器据此区分用户内存与服务内存。
* **延迟清理标志（`dc`）。** 同一个字中的 1 个比特。含义是“块在逻辑上已死，但现在还不能释放”。由 `libsrpc_shmem_free_dc()`（= `tlsf_setdc()`）置位，由垃圾回收器读取。
* **错误码。** 内存函数把分配器码 `TLSF_ERR_*`（1…15）写入线程的错误码（`libsrpc_errno_get()`）。这些编号与系统 `errno` 重叠，因此由 `tlsf_strerror()` 解读，而不是 `libsrpc_strerror()`。
* **分配器替换（可选）。** 库可以拦截 `malloc`、`calloc`、`realloc` 和 `free`，通过 `dlsym(RTLD_NEXT)` 取得原始函数。TLS 中的标志在系统堆和内存池之间切换线程（`libsrpc_alloc_sw_std()` / `libsrpc_alloc_sw_shm()`）。默认不构建拦截代码（`DISABLE_ALLOC=ON`）。

### 4. 垃圾回收器（共享内存 GC）

一个**只存活于守护进程内**的独立线程（由 `libsrpc_shmem_create()` 启动）。它释放那些因其他进程的线程可能仍在读取而不能立即释放的服务块，以及因保持而被推迟清理的用户块。

* **池中的闹钟。** GC 结构——线程、信号量和延迟块列表 `list_head`——位于 `libsrpc_shmem_t` 中，因此任意进程都可通过 `sem_post` 调用 `libsrpc_shm_gc_wakeup()` 唤醒回收器。此外 GC 自己每秒醒来 `SHM_GC_FREQ_CHECK` 次（默认 10）。
* **服务块。** `tlsf_free_uid_blocks(pool, 0, ...)` 遍历堆，把 `dc == 1` 的已分配块收进本地累积器（每轮最多 1024 个）。服务块没有单独的列表：块头标志承担这一角色。
* **占用检查（Hazard Pointer）。** 对每个找到的请求，`libsrpc_req_is_busy_proc()` 检查所有存活进程的 `threads[i].hp_req`。只要有任何一个执行方线程发布了指向该请求的指针，块就保留到下一轮。
* **被推迟的用户块。** 若进程在其 UID 被保持期间死亡，指向其用户块的指针连同 UID 一起进入 `gc.list_head` 的节点。节点用普通 `malloc` 分配在守护进程的地址空间中，不属于内存池。每一轮 GC 都检查是否还有人保持这个 UID。如果没有，节点中的块被释放。
* **批量释放。** 可以释放的块一次成批删除：`tlsf_lock()`、`tlsf_free_nb()` × N、`tlsf_unlock()`。跨进程互斥锁每轮只取一次，而不是每个块一次。

### 5. RPC 机制与 X-macro

样板代码由预处理器生成（`src/macro.h`、`src/argfunc.h`）。

* **RPC_LIST。** `src/libsrpc_rpc_functions.h` 中的 X-macro 是唯一的事实来源。条目：`XF(分派策略, 返回类型, 名字, 参数类型...)`。由它展开原型、标识、包装器、函数表和调度器的 `switch`。
* **标识。** 库内部函数得到 `sRPCFNID_<name>`（编号从 `sRPCFNID_START = 16` 之后开始）和注册表索引 `sRPC_ID2IDX(id)`。在公开的 `libsrpc.h` 中，同一个编号表现为 `libsrpc_funid_<name>`。
* **包装器。** 每个函数生成桩 `librpcimp_<name>`（`sRPCFN(name)`）。它把参数序列化进请求块 `libsrpc_request_t`，分发到各执行方的队列，等待应答，并返回第一个槽的结果。
* **调度。** 在执行方，宏 `M_DECL`、`M_EXTRACT`、`M_ARGNAMES` 把缓冲区拆回栈变量，调用真正的函数，并把结果写入应答槽。`void` 函数的槽只收到就绪标志，没有数据。
* **按返回类型分支。** C 预处理器不能比较类型，因此使用只为 `void` 定义的宏 `COMPARE_void`。`IIF(EQUAL(rettype, void))` 为 `void` 函数和非 `void` 函数展开成不同的代码（`RETDATA`）。
* **构造上的限制。** 不允许变参函数。参数按字节拷贝（`memcpy`），因此传递标量类型和指针，而指针只有指向公共池时才有意义。

### 6. 函数注册

“谁能执行什么”的注册分四步，既不需要 `dlsym`，也不需要在运行时生成桩。

1. **Weak alias（链接）。** 对 `RPC_LIST` 中的每个函数，库声明 `__attribute__((weak, alias("librpcimp_" #name))) rettype name(...);`。应用定义了函数时，强符号覆盖 alias，进程成为**执行方**。否则符号解析到桩，进程成为**调用方**。决定按函数分别作出。
2. **位图（进程启动）。** 表 `srpc_fn[]` 为每个函数保存一对地址：`.rpc`（桩）和 `.loc`（符号实际解析到的地方）。`libsrpc_bmp_func_set()` 比较它们，并在 `srpc_bmp_func_t` 中置上“我能执行”的位。这是两个指针的比较，没有字符串，也没有符号查找。
3. **交给守护进程（连接）。** 位图在 `connect()` 之后、收到内存池 FD 之前，以消息 `regfn_msg_t { size, sign[32], bmp }` 发给守护进程。守护进程检查大小和签名 `"simplerpc_" BUILD_TS`：这防止 `RPC_LIST` 不同的进程对接。
4. **写入注册表（守护进程）。** `libsrpc_reg_func_form_bmp()` 把指向客户端 `libsrpc_proc_t` 的指针插入池中的注册表 `srpc_regfn_shm_t`。客户端从不写注册表。

**注册表结构**（每个函数一条记录）：

* `main_block` —— 结构体内直接放 8 个 `_Atomic(libsrpc_proc_t *)` 槽。典型情况是每个函数只有少数执行方，不需要分配内存。
* `ext_block` —— 主块溢出时分配一个比原来多 32 个槽的扩展，拷贝旧内容，新块经 CAS 发布。注册表只有一个写者，即守护进程的主线程；CAS 保护这次发布。
* 旧的 `ext_block` 不立即释放：其签名被清零，块标记 `dc`，并唤醒 GC。这是一种类似 RCU 的延迟回收，因为其他进程中的读者可能还在遍历旧块。
* `num_all` —— 该函数执行方的原子计数器。调用方读取它以预留应答槽。对外同一数值由 `libsrpc_fnreg_num_get()` 提供。`num_all == 0` 时调用立即以 `ENOREGFUN` 失败。
* 槽的插入和删除用 CAS 完成，没有互斥锁。

**注销**发生在套接字关闭时：`libsrpc_proc_destroy()` → `libsrpc_unreg_func_proc()` 把进程从所有注册表块中移除，并递减 `num_all`。

**链接注意事项。** 一个只执行函数的应用可能不引用 `libsrpc.so` 的任何符号。在链接器标志 `--as-needed` 下，`DT_NEEDED` 依赖会被丢弃，库的构造函数不会执行，进程也不会接入内存池。因此 `example/` 和 `bench/` 使用 `-Wl,--no-as-needed`。

### 7. 进程与线程描述符

守护进程在内存池中为每个连上的客户端创建结构 `libsrpc_proc_t`（块类型 `PROC`）。它是所有发给该客户端的请求的入口。

* **签名与状态。** `sign`（`LIBSRPC_PROC_SIGN`）和 `status` 是原子的。`libsrpc_proc_is_valid()` 要求签名正确且状态为 `RUN`，因此进程失效对其他人立即可见，且无需加锁。
* **`proc_uid`。** 守护进程签发的 16 位标识，也是分配器中的属主 UID。
* **队列与信号量。** 进程有一条入站请求的 MPMC 队列，以及一个供其全部执行方线程使用的唤醒信号量 `sem_wakeup`。
* **执行方线程。** 数组 `threads[]` 位于同一块中，大小为在线 CPU 数（`sysconf(_SC_NPROCESSORS_ONLN)`）。线程按 `sched_getaffinity` 掩码启动，每个可用核心一个，并绑定到该核心（`pthread_start_all_cpu`）。**每一个**加载了库的进程都得到这些线程，包括纯调用方。数组元素保存 `hp_req`——指向正在处理的请求的 Hazard Pointer。
* **请求列表。** `req_head` 是该进程全部请求块的链表。垃圾回收器沿它查找仍被占用的请求，进程清理时沿它检查 UID 保持。

### 8. 同步与队列

0.1.x 版本中自有的 futex（`libsrpc_futex.*`）已删除，同步建立在池中的 POSIX 原语上。

* **MPMC 队列。** `lf_mpmc_queue` 是 `libsrpc_proc_t` 内部按 Vyukov 方案实现的无锁队列（带 `seq` 计数器的单元数组）。单元按缓存行对齐（`alignas(64)`），`head` 和 `tail` 分处不同的行。容量是 2 的幂，构建时指定（`MPMCQ_DEGREE`，默认 `2^10 = 1024`）。单元里只放指向请求的指针，执行方按 `resp->hp_proc` 找到自己的应答槽。
* **入队。** 队列满时调用方重试：16 轮，每轮 32 次并带 `pause` 指令，轮与轮之间 `sched_yield()`。如果始终没有空位，应答槽得到 `-ENOMEM`，请求不发给这个执行方。
* **执行方等待。** 排空队列之后，执行方线程不立即入睡：大约 1024 次迭代里它带着暂停主动查看队列（`DEQUEUE_RETRY`、`libsrpc_cpu_pause(32)`）。之后才在 `threads_wait` 中登记，再查一次队列，然后在 `sem_wakeup` 上入睡。请求密集时，短暂的忙等省去一次唤醒，但每次请求之后都要花费 CPU 时间。
* **唤醒。** 只有存在睡眠线程（`threads_wait > 0`）时，调用方才对进程信号量做 `sem_post`。`sem_t` 以 `pshared = 1` 创建，并包了一层薄的 `libsrpc_sem_*`（`libsrpc_wrapper.h`）。
* **请求体内的信号量。** 每个 `libsrpc_request_t` 有自己的 `sem_wakeup`：应答就绪时，执行方用它唤醒调用线程。
* **自旋锁链表。** `libsrpc_list_spin` 是单链表。写操作在 `pthread_spinlock_t` 下进行，遍历通过带 `acquire` 的原子加载实现无锁。用于进程列表、请求列表和 GC 的延迟块列表。
* **固定大小块池。** `libsrpc_fixblockalloc` 是建立在 FIFO 索引环上、可增长（`nextpool`）的池。守护进程用它在**本地**内存中保存客户端连接描述符。池内没有同步：只有守护进程的 `epoll` 主循环接触它。

### 9. 请求与应答

内存池中的一个块同时包含请求及其全部应答，因此应答不再单独分配内存。

```text
libsrpc_request_t
+----------------------------------------------------------------------+
| node | sem_wakeup | hp_regfn | sign | seq_num | funid | bufsz |      |
| retoff | retsz | retnum | gc_lock_uid |                              |
+----------------------------------------------------------------------+
| buf[]:                                                               |
|   [参数 .......]  <- retoff = ALIGNLONG(len(args))                    |
|   [libsrpc_response_t #0][ret]  <- 每个大小 retsz                      |
|   [libsrpc_response_t #1][ret]                                        |
|   ...                          <- 共 retnum 个槽位                     |
+----------------------------------------------------------------------+

libsrpc_response_t: { hp_proc (等待谁应答), rc, buf[] }
```

* **请求块缓存。** 指向线程最后一个块的指针保存在 TLS（`current_req`）中。新请求放得下就复用该块，不触碰分配器。放不下就按两倍余量分配新块。旧块立即释放；若其中还留着未写完或已超时的应答，则经 `dc` 标志和垃圾回收器释放。热路径上根本不调用分配器。同一指针还服务于 `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()`。块作为线程数据注册（`pthread_key_create`），线程结束时释放。
* **UID 保持（`gc_lock_uid`）。** 请求体中有四个 16 位槽。`libsrpc_shmem_proc_lock(idx, ptr)` 取应答槽 `idx` 的执行方。若 `tlsf_check_uid()` 确认 `ptr` 属于其 `proc_uid`，就把该 UID 写入空闲槽，函数返回槽号（`0…3`）。缓存块被替换时掩码复制到新块，复用时不清除。只要该 UID 还出现在某个存活进程的至少一个请求中，`libsrpc_proc_destroy()` 就不会立即释放该 UID 的用户块。保持绑定在线程上：由取走它的线程解除，并随线程或进程一起消失。
* **就绪协议。** 执行方用 CAS 把标志 `LIBSRPC_RC_FLAG_LOCK`（`0xC0000000`）写入 `rc`，拷贝结果，再发布 `LIBSRPC_RC_FLAG_READY`（`0x80000000`）。负的 `rc` 是槽的错误码，`0` 表示还没有应答。包装器只在与就绪标志精确匹配时接受应答。槽仍处于 `LOCK` 状态时，它最多等待 100 次 `pause`。
* **校验。** `libsrpc_req_is_corrupted()` 以两次带 `acquire` 的读取检查签名，一次在读字段之前，一次在之后，并检查尺寸是否一致。`libsrpc_req_destroy()` 通过 CAS 清除签名，因此重复销毁是安全的。这保护了 GC 与执行方同时工作时的 use-after-free。
* **分派策略。** 注册表按顺序遍历：先 `main_block`，后 `ext_block`。
  * `RPC_SEND_ALL` —— 请求发给所有执行方（`retnum = num_all`）。
  * `RPC_SEND_FIRST` —— 发给第一个接受请求的执行方（`retnum = 1`）。
  * `RPC_SEND_LAST` —— 发给遍历中最后一个非空槽。
  * `RPC_SEND_RR` —— 按轮询发给一个执行方（原子计数器 `req_send_rr` 对 `num_all` 取模）。
  * `LAST` 和 `RR` 没有后备执行方：所选进程没有接受请求时，调用以 `ENOTAVAILABLE` 结束。
* **部分分派。** 实际发出的请求少于预留的槽时，期望的应答数相应减少。一个都发不出去时，调用以 `ENOTAVAILABLE` 结束。
* **超时。** 调用方在 `libsrpc_sem_wait_timeout_us()` 循环中等待，每次唤醒后重新统计已就绪的槽。间隔以微秒设置：
  * `libsrpc_timeout_oneshot_set()` —— 线程的下一次调用；
  * `libsrpc_timeout_func_set()` —— 某个函数，限于该线程；
  * `libsrpc_timeout_global_set()` —— 整个进程。

  默认 `LIBSRPC_TIMEOUT_DEFAULT`（1 秒），下限 `LIBSRPC_TIMEOUT_MINIMUM`（100 微秒）。每次唤醒后间隔重新计时。若在间隔内没有新应答到达，空槽被标记为 `-ERPCWAITIMEDOUT`，等待结束。

### 10. 递归与错误

* **禁止递归。** 执行方线程中设置了 TLS 标志 `srpc_disable_rpc_recursion`，因此执行方不能从处理函数中自己发起 RPC 调用：请求不会发出，线程的码是 `ERECURSIVE`。
* **调度器中的检查。** 调用前比较 `&name` 与 `&sRPCFN(name)` 的地址。二者相等说明该进程在没有本地实现的情况下进入了注册表，桩将会调用自身。这样的调用不会执行，槽一直没有应答，直到超时。
* **自有错误码。** `libsrpc` 以 `MAX_ERRNO`（4095）之后的码扩展系统 `errno`：

| 码 | 值 | 含义 |
|---|---|---|
| `ELIBNOINIT` | 4096 | 库未初始化 |
| `ESHMNOINIT` | 4097 | 内存池未接入 |
| `ERPCDISABLE` | 4098 | RPC 已关闭：与守护进程失去联系 |
| `ENOREGFUN` | 4099 | 函数没有执行方 |
| `ERECURSIVE` | 4100 | 禁止从执行方线程发起 RPC |
| `ECANCELLED` | 4101 | 执行方在取走请求之前断开 |
| `ENOTAVAILABLE` | 4102 | 没有可用的执行方或槽 |
| `ERPCWAITIMEDOUT` | 4103 | 超时内没有应答 |
| `ENOTFOUND` | 4104 | 块不属于该执行方（保持） |
| `EPROCDESTROYED` | 4105 | 执行方已经断开（保持） |
| `EINVALREQUEST` | 4106 | 没有可用的最近一次请求（保持） |
| `EINVALRESPONSE` | 4107 | 没有可用的应答槽（保持） |

* **码放在哪里。** 线程的错误码存在 TLS 变量中。包装器在调用开始时清零，并在请求发不出去时记下原因。经 `libsrpc_errno_get()` 读取，由 `libsrpc_strerror()` 解读。已发出请求的结局在其槽的 `rc` 中，由 `libsrpc_lastreq_get()` 返回。超时和取消不进入线程的码。

## 🔄 RPC 调用的生命周期

1. **初始化。**
   * 应用加载 `libsrpc.so`，优先级 101 的构造函数执行。
   * 构造函数经 `dlsym(RTLD_NEXT)` 取得 `malloc`/`calloc`/`realloc`/`free` 的原始函数。它按 `program_invocation_name` 检查当前进程是不是守护进程本身。如果不是，就拉起守护进程（在 `bind()` 竞争中失败的守护进程静默退出）并启动 `unix_client` 控制线程。
   * 线程连接守护进程的套接字，发送自己的函数位图，接收内存池 FD，并把它映射到固定地址。
   * 线程找到自己的 `libsrpc_proc_t` 和 `proc_uid`，按可用 CPU 数量启动执行方线程，把状态置为 `RUN` 并允许 RPC。构造函数一直等到进程就绪才交还控制权。

2. **调用（调用方一侧）。**
   * 应用调用 `log_write("Hello")`。符号解析到生成的桩 `librpcimp_log_write`。
   * 桩从注册表读取 `num_all`。没有执行方时，调用立即以 `ENOREGFUN` 结束。
   * 取用缓存的 `libsrpc_request_t`，或分配新的（`libsrpc_shmem_malloc_type(..., LIBSRPC_SHMDT_REQUEST)`）。
   * 参数拷贝进 `req->buf`，并划出应答槽。
   * 对每个选中的执行方填入 `resp->hp_proc`，请求放入其 MPMC 队列。执行方有睡眠线程时，对进程信号量做 `sem_post`。
   * 调用线程在 `req->sem_wakeup` 上入睡。

3. **处理（执行方线程）。**
   * 绑定在自己核心上的执行方线程从队列取出请求：或者在短暂的忙等期间，或者在进程信号量上醒来之后。
   * 发布 Hazard Pointer `thread->hp_req`，并再次检查请求签名。
   * 按 `hp_proc` 找到自己的槽，拆开参数，调用真正的 `log_write()`。
   * 把结果写入槽：`LOCK` → 数据 → `READY`。

4. **结束。**
   * 执行方对 `req->sem_wakeup` 做 `sem_post`，并撤下 Hazard Pointer。
   * 调用方醒来，重新统计已就绪的槽。全部收齐后，包装器返回槽 0 的结果。其余的通过 `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()` 取得。
   * 请求块留在线程缓存中供下一次调用使用。

## 💥 故障与恢复

* **进程在分配器操作中途死亡。** 下一次取得内存池互斥锁时收到 `EOWNERDEAD`，并按 BUL 回滚未完成的操作。块内的应用数据不回滚：应用放在内存池中的结构由应用自己保护，例如用带 `EOWNERDEAD` 处理的健壮互斥锁。
* **执行方死亡。** 守护进程看到套接字断开并调用 `libsrpc_proc_destroy()`：
  * 进程从注册表中移除，其描述符失效；
  * 执行方来不及从队列取走的请求得到 `-ECANCELLED`，调用方被唤醒；
  * 执行方已经在处理的请求一直没有应答，调用方在超时后得到 `-ERPCWAITIMEDOUT`；
  * 进程的块由 `tlsf_free_uid_blocks(pool, proc_uid, ...)` 遍历处理。没有人保持其 UID 时，用户块立即释放，否则推迟到 GC 列表。请求块被作废并标记 `dc`，进程描述符经 `libsrpc_shmem_free_dc()` 释放。
* **调用方死亡。** 它的请求块被作废并交给 GC。若签名在执行方取走请求之前就被清掉，函数不会被调用。若发生在处理期间，函数会执行，但结果不会写入。执行方的 Hazard Pointer 撤下之后，GC 释放该块。
* **保持的持有者死亡。** 保持检查只看存活进程的请求，而线程的请求块在线程结束时释放。因此保持随持有者一起消失，被推迟的块在下一轮 GC 归还内存池。死亡进程中忘记的 `unlock` 不会变成永久泄漏。
* **执行方挂起。** 调用方在超时后得到 `-ERPCWAITIMEDOUT`。下一次调用时，带有过期槽的块被新块替换，旧块交给 GC。
* **守护进程死亡。** 客户端看到套接字断开：它们的 RPC 被关闭（`rpc_enable = false`），执行方线程停止。没有重连。旧内存池的映射仍留在进程中，但新进程会拉起一个带新内存池的新守护进程，与已经在运行的进程互不相见。守护进程是单一故障点。

## 📊 交互示意图

```text
                        +--------------------------------+
                        |  Guard Daemon (memfd/fexecve)  |
                        |  - abstract unix socket        |
                        |  - owns SHM (memfd)            |
                        |  - registry writer             |
                        |  - GC thread                   |
                        +--------------------------------+
                          ^  SCM_RIGHTS (shm fd)      ^
                          |  regfn bitmap             |  regfn bitmap
+-------------------+     |                           |     +-------------------+
|   Process A       |-----+                           +-----|   Process B       |
|  (RPC Client)     |                                       |  (RPC Executor)   |
+-------------------+                                       +-------------------+
          |                                                           |
          |  1. log_write("Msg")                                      |
          v                                                           v
+-------------------+          Shared Memory @ SHMEM_BASE_VADR   +-------------------+
| RPC Wrapper       |  +--------------------------------------+   | Worker Thread     |
| - read num_all    |<-| regfn: main_block[8] + ext_block[+32]|   | (pinned to CPU)   |
| - Alloc Request   |  +--------------------------------------+   | - Publish hp_req  |
| - Serialize Args  |  | tlsf_txn pool (uid / data_type / dc) |   | - Deserialize     |
| - Enqueue ========|=>| proc_B: MPMC Queue + sem_wakeup       |==>| - Call Function   |
| - Sem Wait        |  | request: [args][resp#0][resp#1]...    |   | - Write Result    |
+-------------------+  +--------------------------------------+   +-------------------+
          ^                                                           |
          |                                                           |
          +-----------------------------------------------------------+
                  2. sem_post(req->sem_wakeup) / Result Read
```

## ⚙ 实现细节

* **匿名内存（memfd）。** 内存池用 `memfd_create` 创建，而不是 `shm_open`。它在 `/dev/shm` 中不可见，描述符的最后一次引用消失后什么也不留下：崩溃之后不需要手工清理。
* **固定基地址。** 内存池内部是**绝对**指针，因此所有进程都以 `MAP_FIXED_NOREPLACE` 把它映射到 `SHMEM_BASE_VADR`（默认 `0x200000000000`）。地址已被占用时，映射老老实实创建失败，而不是覆盖别人的区域。
* **防 fork。** 对内存池调用 `madvise(..., MADV_DONTFORK)`：子进程不继承内存池，也不能意外破坏分配器的元数据。`fork()` 之后子进程同样不能使用 RPC 和内存池。
* **多核。** 执行方线程按允许的 CPU 数量创建，并通过 `pthread_attr_setaffinity_np` 绑定到核心，从而减少上下文切换和缓存迁移。
* **通过 BUILD_TS 隔离构建。** 配置时间戳只编译进 `libsrpc.so`，并进入守护进程名、套接字名、内存池签名和注册消息签名。这实际上是自动生成的 ABI 版本：加载了不同库构建的进程互相看不见。应用本身不含这个戳。`RPC_LIST` 变化时仍需重新构建应用，因为原型和 `libsrpc_funid_<name>` 来自它。
* **构建参数。** 内存池大小（`SHMEM_SIZE_KB`）、基地址（`SHMEM_BASE_VADR`）、队列容量（`MPMCQ_DEGREE`）、消息级别（`DBG_LVL`）和关闭分配器替换（`DISABLE_ALLOC`）由 CMake 变量指定，并以 `-D` 定义进入代码。
* **严格的编译标志。** 分配器以 `-Wall -Wextra -Werror -Wformat` 构建，库以 `-Wall -Wextra -Werror -Wconversion -Wshadow` 构建。未使用的参数用 `__attribute__((unused))` 标记。
* **绑定 x86_64。** 忙等使用 `__builtin_ia32_pause`，库以 `-mrdrnd` 构建。在其他架构上，代码不做修改就无法构建。

## ⚠️ 已知限制

* **超时从最近一次应答起算。** 分派给多个执行方时，每到一个应答就重新开始计时。最坏情况下，总等待时间接近执行方数量乘以超时。
* **没有发出的调用可能返回旧值。** 请求发不出去时（`ERPCDISABLE`、`ERECURSIVE`、`ENOTAVAILABLE`），包装器读取被复用块的槽 0。里面可能还留着该线程上一次调用的结果，`libsrpc_lastreq_get(0)` 对它也返回 `0`。对有返回值的函数，原因码有时会被替换成 `ETIMEDOUT`。成功的可靠标志是 `libsrpc_errno_get() == 0` **并且**所需槽为 `0`。
* **内存函数的错误码。** 编号与系统 `errno` 相同的 `TLSF_ERR_*` 会进入线程的码。`libsrpc_strerror()` 会把它们解错，需要 `tlsf_strerror()`。
* **不检查客户端权限。** `SO_PEERCRED` 中只取 PID。任何以正确签名连上抽象套接字的进程都会得到整个内存池的可读写 FD。
* **守护进程是单一故障点。** 没有向新守护进程重连（见「故障与恢复」）。
* **没有内存池时的 `libsrpc_fnreg_num_get()`。** 函数不检查内存池是否已接入。与守护进程的连接失败时，调用会访问并不存在的内存。
* **忙等的代价。** 每个进程都按 CPU 数量保持执行方线程，每次请求之后它们在入睡前都会转一会儿。
* **固定的内存池。** 内存池大小在构建时确定，不会增长。同时接入的进程数受 16 位 `proc_uid` 限制。

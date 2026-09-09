# simplerpc 架构

本文档描述 `libsrpc` 库的内部结构、组成部件以及各部件之间的交互机制。

## 🏗 总体理念

**simplerpc** 是一个基于共享内存（Shared Memory）的透明 RPC 框架。与经典的 RPC 系统（将参数序列化为字节流——Protobuf、JSON——再经网络传输）不同，`simplerpc` 直接在所有进程以同一虚拟地址映射的公共内存段中传递调用参数，从而避免了跨地址空间的拷贝。

### 核心原则
1. **零配置守护进程（Zero-Conf Daemon）：** 后台协调进程在库被任何应用加载时自动启动，并在最后一个客户端断开后退出。
2. **内存为中心（Memory-Centric）：** 所有数据、进程描述符、函数注册表和消息队列都存放在由事务型 TLSF 分配器管理的共享内存中。
3. **链接期分派（Link-Time Dispatch）：** 进程的角色（调用方或执行方）由链接器通过 `weak alias` 决定，而非配置或运行时注册。没有 IDL，也没有代码生成器——一切由单个 X-macro 经预处理器展开。
4. **基于所有权的清理（Ownership-Based Cleanup）：** 每个内存块都带有属主进程的 UID 标记，因此对崩溃或断开的进程，其名下的一切都可以被完整回收，无需维护外部分配登记簿。
5. **透明 IPC（可选）：** 替换标准函数（`malloc`、`free`）可使现有代码无需修改即可操作共享内存。默认通过构建选项 `DISABLE_ALLOC=ON` 关闭。

## 🧩 主要组件

### 1. Guard Daemon（后台守护进程）
协调进程，负责管理内存池、函数注册表和进程描述符的生命周期。RPC 流量不经过守护进程——它只是资源的所有者和注册的"公证人"。

* **启动：** 由 `spawn_daemon()` 函数实现。守护进程通过双重 `fork()` 和 `setsid()` 创建，成为无控制终端的孤儿进程，且不会在调用方留下僵尸进程。
* **嵌入式加载器（Embedded Loader）：** 守护进程的可执行文件不落盘。它以 C 数组形式（由 `src/libsrpc_loader.c` 经 `xxd -i -n loader` 生成）内嵌在 `libsrpc.so` 库本身中，写入 RAM 中的临时 `memfd`，再通过 `fexecve()` 启动。加载器通过环境变量 `LIBSIMPLERPC_SO` 获得库的路径（其值经 `dladdr()` 取得），执行 `dlopen()` 后将控制权交给 `simplerpc_daemon_main()`。
* **为什么用独立进程而不是线程：** 守护进程需要干净的地址空间——它以 `MAP_FIXED_NOREPLACE` 将内存池映射到固定地址 `SHMEM_BASE_VADR`，且不应承载父应用的分配和线程。
* **领导者选举：** *每个*进程都会尝试拉起守护进程，但抽象 Unix socket 的 `bind()` 是原子的：失败者收到 `EADDRINUSE` 并静默退出。客户端在 `ECONNREFUSED` 时重试 `connect()`（约 1 秒内）。
* **职责：** 创建抽象 Unix socket，分配共享内存，向连接的客户端分发内存文件描述符（FD），维护 RPC 函数注册表，并启动垃圾回收线程。
* **退出：** 主循环为 `epoll_wait(timeout = 1 s)`；当客户端计数降为零时，守护进程退出，共享内存（匿名 `memfd`）随最后一个引用一并消失。

### 2. 传输层（Unix Sockets & SCM_RIGHTS）
仅用于信令交互、函数注册和建立连接。

* **抽象 socket：** socket 名称形如 `"simplerpc_" BUILD_TS`，其中 `BUILD_TS` 是构建时间戳。链接不同构建版本库的应用会落入互不交叉的"域"。
* **连接顺序（`libsrpc_unix_server_addclient`）：** `accept4()` → `SO_PEERCRED`（获取客户端 PID）→ `libsrpc_proc_create(pid)` → `SO_RCVTIMEO = 5 s` → 读取注册消息 → 注册函数 → 传递内存 FD → 加入 `epoll`。
* **客户端-服务器：** 只有在注册成功之后，守护进程才通过 `SCM_RIGHTS` 机制（ancillary data）向客户端传递共享内存的文件描述符（FD）。未通过签名校验的客户端永远无法访问内存池。
* **mmap：** 客户端拿到 FD 后，以事先约定的虚拟地址（`SHMEM_BASE_VADR`）和 `MAP_FIXED_NOREPLACE` 标志对守护进程所映射的同一内存区域执行 `mmap()`。这保证了一个进程中创建的指针在另一个进程中依然有效。此外还会校验池头部的签名。
* **死亡检测：** 连接断开（`EPOLLHUP | EPOLLRDHUP` 或 `recv() == 0`）是客户端已消失的唯一且充分的标志。内核会关闭 socket，因此无论是 `exit()`、`SIGKILL` 还是 segfault，事件都会送达。
* **客户端控制线程：** 应用侧由一个独立的 detached 线程 `unix_client` 负责处理该连接，它完成全部初始化（注册、`mmap`、启动执行方），然后挂起在 `epoll_wait` 中。退出唤醒通过 `eventfd` 实现。

### 3. 内存管理（TLSF 共享内存）

* **tlsf_txn v4.1：** 事务型 TLSF 分配器，源码直接包含在项目树中（`libs/tlsf_txn/`，无 git 子模块）。依靠 `FL × SL` 空闲链表矩阵和位图实现确定性的 O(1) 块分配。内存池格式带版本号（`TLSF_FORMAT_VERSION`），外来格式会被拒绝。
* **分配器内部的健壮互斥锁（Robust Mutex）：** 进程间互斥由 `pthread_mutex_t` 提供，属性为 `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST | PTHREAD_MUTEX_RECURSIVE`，*内嵌*于内存池控制结构中。`libsrpc` 自身不直接加锁——它只调用 `tlsf_*`（与 0.1.x 版本不同，那时互斥锁位于 libsrpc 结构中，每次操作都需手动加锁）。
* **BUL（Binary Undo Log）：** 每次写入 8 字节元数据字之前，旧值都会保存到池内的回滚栈。操作成功即提交（重置栈顶）。若互斥锁属主死亡，下一次 `tlsf_lock()` 会收到 `EOWNERDEAD`，自动回滚未完成的事务（`bul_recover`），并调用 `pthread_mutex_consistent()`。无需外部看门狗，内存池始终保持一致。
* **所有权模型（UID）：** 每个块头含 `uid[12]` 数组。只有当所有属主都解除关联后，块才会归还内存池。`libsrpc` 向分配器传递 `proc_uid`——由守护进程签发的 16 位进程标识（守护进程自身为 `DAEMON_PROC_UID = 1`）。`libsrpc_shmem_link()` 允许进程将自己加为其他进程的块的属主，从而保护它不被释放。
* **块的类型化：** 块头中有一个 15 位的 `data_type` 字段，存储 `libsrpc_shm_data_type_t` 中的一种类型：`USER`（应用数据）、`PROC`（进程描述符）、`REGFN`（函数注册表块）、`REQUEST`（RPC 请求块）。垃圾回收器和析构函数依据该字段区分用户内存与服务内存。
* **延迟清理标志（`dc`）：** 同一头部字段中的 1 个比特。它充当 retire 标记："块在逻辑上已死，但当前不能释放"。由 `libsrpc_shmem_free_dc()`（= `tlsf_setdc()`）置位，由垃圾回收器读取。
* **分配器劫持（Allocator Hijacking，可选）：** 库可以拦截标准函数 `malloc`、`calloc`、`realloc` 和 `free`，并通过 `dlsym(RTLD_NEXT)` 取得原始函数。借助 `Thread Local Storage`（TLS），线程可以在系统分配器与共享内存分配器之间"热切换"（`libsrpc_alloc_sw_std()` / `libsrpc_alloc_sw_shm()`）。默认通过 `DISABLE_ALLOC=ON` 将整段代码从构建中剔除。

### 4. 垃圾回收器（共享内存 GC）
一个**只存活于守护进程内**的独立线程（由 `libsrpc_shmem_create()` 初始化），负责回收那些因其他进程中可能存在读者而不能立即释放的服务结构。

* **共享内存中的闹钟：** GC 结构（线程 + 信号量）位于 `libsrpc_shmem_t` 中，因此任意进程都可通过 `sem_post` 调用 `libsrpc_shm_gc_wakeup()` 唤醒回收器。此外 GC 还按超时自动醒来，频率为每秒 `SHM_GC_FREQ_CHECK` 次（默认 10）。
* **垃圾查找：** `tlsf_free_uid_blocks(pool, 0, ...)` —— 对堆做物理遍历，寻找带 `dc == 1` 标志的已分配块。没有单独的延迟对象列表：块头标志就承担了这一角色。
* **占用检查（Hazard Pointers）：** 找到的块被收集进本地累积池，随后对每个请求调用 `libsrpc_req_is_busy_proc()`，它会遍历所有存活的 `libsrpc_proc_t` 及其全部 `threads[i].hp_req`。只要有任何一个执行方线程发布了指向该请求的指针，该块就保留到下一轮。
* **批量释放：** 空闲块在单次 `tlsf_lock()` / `tlsf_free_nb()` × N / `tlsf_unlock()` 之下成批删除——避免为每个块都抓取跨进程互斥锁。
* **崩溃进程善后：** 连接断开时守护进程调用 `libsrpc_proc_destroy()`，它将进程从函数注册表中移除、使其描述符失效、取消挂起的请求（`-ECANCELLED`），并调用 `tlsf_free_uid_blocks(pool, proc_uid, ...)`——按属主 UID 直接遍历堆。用户块立即释放，服务块标记 `dc` 交由 GC 处理。

### 5. RPC 机制与宏（X-Macro）
为消除样板代码，使用了 C 预处理器（`src/macro.h`、`src/argfunc.h`）。

* **RPC_LIST：** 位于 `src/libsrpc_rpc_functions.h` 的 X-macro，唯一的事实来源。条目格式：`XF(分派策略, 返回类型, 名称, 参数类型...)`。原型、标识符、包装器、函数表以及调度器的 `switch` 都由它展开。
* **标识符：** 每个函数获得唯一 ID `sRPCFNID_<name>`（编号从 `sRPCFNID_START = 16` 起）以及注册表索引 `sRPC_ID2IDX(id)`。
* **包装器生成：** 编译期生成桩函数（`sRPCFN(name)` → `librpcimp_<name>`），它们将参数序列化进请求缓冲区（`libsrpc_request_t`），并将其放入执行方的 MPMC 队列。
* **分派：** 执行方一侧，宏 `M_DECL`、`M_EXTRACT`、`M_ARGNAMES` 自动把缓冲区反序列化回栈变量并调用原始函数，然后将结果拷入应答槽。
* **按返回类型的编译期分支：** 由于 C 不允许在预处理器中比较类型，使用了仅对 `void` 定义的宏 `COMPARE_void` 的技巧：`IIF(EQUAL(rettype, void))` 针对 `void` 与非 `void` 函数展开出不同的代码（`RETDATA`）。
* **设计限制：** 不允许可变参数函数；参数按字节拷贝（`memcpy`），因此只允许标量类型和指针，且指针只有在指向共享池时才有意义。

### 6. RPC 函数的动态链接
"谁能执行什么"的注册分四个阶段完成，既不需要 `dlsym`，也不需要运行时生成桩代码。

1. **Weak alias（链接阶段）。** 对 `RPC_LIST` 中的每个函数，库声明 `__attribute__((weak, alias("librpcimp_" #name))) rettype name(...);`。若应用自行定义了该函数——强符号覆盖 alias，进程即成为**执行方**。若没有——符号解析到桩函数，进程即成为**调用方**。该决定对每个函数独立作出：同一进程可以是某些函数的执行方，同时又是另一些函数的调用方。
2. **位图（进程启动）。** 表 `srpc_fn[]` 为每个函数存一对地址：`.rpc`（桩函数地址）和 `.loc`（符号实际解析到的地址）。`libsrpc_bmp_func_set()` 函数比较二者并在 `srpc_bmp_func_t` 中置位——"我会执行这个"。只是两个指针的比较，没有字符串，也没有运行时解析。
3. **传给守护进程（连接时）。** `connect` 之后、收到内存 FD 之前，位图通过 Unix socket 以 `regfn_msg_t { size, sign[32], bmp }` 消息发送给守护进程。守护进程校验大小和签名（`"simplerpc_" BUILD_TS`）——防止不同版本 `RPC_LIST` 的应用相互对接。
4. **注册表录入（守护进程）。** `libsrpc_reg_func_form_bmp()` 把客户端的 `libsrpc_proc_t` 指针插入位于共享内存中的 `srpc_regfn_shm_t` 注册表。客户端自身从不写注册表。

**注册表结构**（`srpc_regfn_shm_t`，每个函数一条记录）：

* `main_block` —— 结构体内部直接内置 8 个 `_Atomic(libsrpc_proc_t *)` 槽位，无需任何分配。典型场景（每个函数只有少数执行方）零内存分配即可满足。
* `ext_block` —— 主块溢出时分配 `+32` 槽位的扩展块，拷贝旧内容，并通过 `atomic_compare_exchange_strong` 发布新块。竞争失败者释放自己的块并重试。
* 旧的 `ext_block` 不立即释放：其签名被清零，块标记 `dc`（`libsrpc_shmem_free_dc`）并唤醒 GC——一种 RCU 式的延迟回收方案。
* `num_all` —— 该函数已注册执行方的原子计数器。客户端准备请求时读取它，以决定预留多少个应答槽。若 `num_all == 0`，调用立即以 `-ENOREGFUN` 失败。
* 槽位的插入和删除全部用 CAS 操作完成，不使用任何互斥锁。

**注销**发生在 socket 关闭时：`libsrpc_proc_destroy()` → `libsrpc_unreg_func_proc()` 将进程从所有 `main_block`/`ext_block` 中移除，并递减 `num_all`。

**链接注意事项：** 一个*仅*导出 RPC 函数的应用，可能对 `libsrpc.so` 的任何符号都没有引用。在链接器默认标志（`--as-needed`）下，`DT_NEEDED` 条目会被丢弃，库的构造函数不会执行，进程甚至根本不会接入内存池。因此 `example/` 和 `bench/` 中必须加 `-Wl,--no-as-needed`。

### 7. 进程与线程描述符
`libsrpc_proc_t` 结构体由守护进程为每个连接的客户端在共享内存中创建（块类型 `LIBSRPC_SHMDT_PROC`），是所有发往该进程请求的入口点。

* **签名与状态：** `sign`（`LIBSRPC_PROC_SIGN`）和 `status` 均为原子量。`libsrpc_proc_is_valid()` 要求签名正确且状态为 `RUN`，因此对进程的作废对所有其他进程立即可见，无需加锁。
* **`proc_uid`：** 由守护进程签发的 16 位标识；用作 TLSF 分配器中的属主 UID。
* **队列与信号量：** 每个进程拥有自己的 MPMC 队列，以及一个供其全部执行方线程共用的唤醒信号量 `sem_wakeup`。
* **线程数组：** `threads[]` 位于同一内存块中，大小按在线 CPU 数（`sysconf(_SC_NPROCESSORS_ONLN)`）确定。工作线程本身按 `sched_getaffinity` 掩码启动（`pthread_start_all_cpu`）。每个元素含 `hp_req`——指向正在处理请求的 Hazard Pointer。
* **请求列表：** `req_head` —— 该进程全部请求块的链表，供垃圾回收器遍历。

### 8. 同步与队列
0.1.x 版本中自研的 futex 实现（`libsrpc_futex.*`）已删除；同步机制建立在共享内存中的 POSIX 原语之上。

* **MPMC 队列：** 无锁队列 `lf_mpmc_queue`（Vyukov 方案：带 `seq` 计数器的单元数组），置于共享内存中的 `libsrpc_proc_t` 内。用于把任务从客户端交给执行方的工作线程。单元按缓存行对齐（`alignas(64)`），`head` 与 `tail` 分置于不同缓存行。容量为 2 的幂，构建时设定（`MPMCQ_DEGREE`，默认 `2^10 = 1024`）。单元中只存放指向共享内存中 `libsrpc_request_t` 的指针；工作线程通过 `resp->hp_proc` 找到自己的应答槽。
* **POSIX 信号量：** 以 `pshared = 1` 初始化并置于共享内存的 `sem_t` 信号量，用于在队列无任务时使工作线程休眠与唤醒（节省 CPU）。外面包了一层薄薄的 `libsrpc_sem_*` 封装（`libsrpc_wrapper.h`），以便必要时替换实现。唤醒是按需的：存在等待线程（`threads_wait > 0`）或进程恰好只有一个工作线程（`threads_run == 1`）时才调用 `sem_post`。
* **请求体内的信号量：** 每个 `libsrpc_request_t` 都自带一个 `sem_wakeup`——执行方用它通知调用线程应答已就绪。它取代了早期版本的"Job Futex"。
* **自旋锁链表：** `libsrpc_list_spin` —— 单链表，写操作在 `pthread_spinlock_t` 保护下进行，遍历通过带 `acquire` 语义的原子加载实现无锁。用于进程列表和请求列表。
* **定长块池：** `libsrpc_fixblockalloc` —— 基于 FIFO 索引环的快速池，支持自动扩容（`nextpool`），供守护进程在*本地*（非共享）内存中管理客户端连接描述符。它不含同步机制，按单线程使用设计——访问仅来自守护进程的主 `epoll` 循环。

### 9. 请求结构与应答协议
共享内存中的一个块同时包含请求及其全部应答，因此应答无需额外分配。

```text
libsrpc_request_t
+----------------------------------------------------------------------+
| node | sem_wakeup | hp_regfn | sign | seq_num | funid | bufsz |      |
| retoff | retsz | retnum |                                            |
+----------------------------------------------------------------------+
| buf[]:                                                               |
|   [参数 .......]  <- retoff = ALIGNLONG(len(args))                    |
|   [libsrpc_response_t #0][ret]  <- 每个大小 retsz                      |
|   [libsrpc_response_t #1][ret]                                        |
|   ...                          <- 共 retnum 个槽位                     |
+----------------------------------------------------------------------+

libsrpc_response_t: { hp_proc (等待谁应答), rc, buf[] }
```

* **请求块缓存：** 指向最后一个块的指针保存在 TLS（`current_req`）中。若新请求放得下——直接复用该块，不触碰分配器；放不下——释放旧块并按两倍余量分配新块。热路径上根本不调用分配器。同一指针还服务于 `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()`。
* **就绪协议：** 执行方先置 `LIBSRPC_RC_FLAG_LOCK`（`0xC0000000`），拷贝结果后再发布 `LIBSRPC_RC_FLAG_READY`（`0x80000000`），或写入负的错误码。包装器中的客户端仅在与就绪标志精确匹配时才认为应答有效。
* **校验：** `libsrpc_req_is_corrupted()` 检查签名（在读取字段前后各做一次带 `acquire` 语义的读取）以及尺寸的一致性。`libsrpc_req_destroy()` 通过 CAS 清除签名，因此重复销毁是安全的——这保护了 GC 与执行方并发工作时的 use-after-free。
* **分派策略：** `RPC_SEND_ALL` —— 请求发给所有已注册的执行方（`retnum = num_all`）；`RPC_SEND_FIRST` —— 发给第一个可用的（`retnum = 1`）；`RPC_SEND_LAST` —— 发给遍历 `main_block` / `ext_block` 时的最后一个；`RPC_SEND_RR` —— 按轮询发给一个执行方（原子计数器 `req_send_rr`）。
* **部分成功：** 若实际发出的请求数少于执行方数量，则期望的应答数相应下调；若一个都没发出去——调用方收到 `-ENOTAVAILABLE`。
* **超时：** 等待应答是循环调用 `libsrpc_sem_wait_timeout_us`，每次唤醒后重新统计已就绪的槽位。间隔以微秒设置：`libsrpc_timeout_oneshot_set()`（下一次调用）、`libsrpc_timeout_func_set()`（指定函数）、`libsrpc_timeout_global_set()`（所有调用）；默认 `LIBSRPC_TIMEOUT_DEFAULT`（1 秒），下限 `LIBSRPC_TIMEOUT_MINIMUM`（100 微秒）。若在该间隔内未收齐应答，空槽被标记为 `-ERPCWAITIMEDOUT`。

### 10. 递归防护与错误处理
* **TLS 标志：** 执行方线程中设置了 `srpc_disable_rpc_recursion`，因此执行方不能自行发起 RPC 调用——尝试将返回 `-ERECURSIVE`。
* **调度器中的检查：** 调用前比较 `&name` 与 `&sRPCFN(name)` 的地址；若相等，说明该进程在没有本地实现的情况下进入了注册表，桩函数将会调用自身。
* **错误码：** `libsrpc` 以自身错误码扩展系统 `errno`，编号紧随 `MAX_ERRNO` 之后：`ELIBNOINIT`、`ESHMNOINIT`、`ERPCDISABLE`、`ENOREGFUN`、`ERECURSIVE`、`ECANCELLED`、`ENOTAVAILABLE`、`ERPCWAITIMEDOUT`。它们存于 TLS 变量中，经 `libsrpc_errno_get()` 获取（`void` 函数无法返回错误码，故需要它），并由 `libsrpc_strerror()` 解码。

## 🔄 RPC 调用的生命周期

1. **初始化：**
   * 应用加载 `libsrpc.so`；优先级为 101 的构造函数执行。
   * 构造函数通过 `dlsym(RTLD_NEXT)` 取得原始 `malloc`/`free`，并依据 `program_invocation_name` 检查当前进程是否为守护进程。若不是——fork 出守护进程（在 `bind()` 竞争中落败的守护进程会静默退出）。
   * 启动控制线程 `unix_client`：连接守护进程的 Unix socket，发送本地已实现函数的位图，接收内存 FD，并按固定地址执行 `mmap`。
   * 客户端取出自己的 `libsrpc_proc_t` 与 `proc_uid`，为每个可用 CPU 核心启动一个工作线程，置 `RUN` 状态并开启 RPC（`rpc_enable`）。

2. **函数调用（客户端侧）：**
   * 用户调用 `log_write("Hello")`；符号解析到生成的包装器 `librpcimp_log_write`。
   * 包装器从注册表读取 `num_all` —— 若没有执行方，立即返回 `-ENOREGFUN`。
   * 在共享内存中分配（或复用缓存的）`libsrpc_request_t`，经 `libsrpc_shmem_malloc_type(..., LIBSRPC_SHMDT_REQUEST)`。
   * 参数拷入 `req->buf`，布置应答槽。
   * 对 `main_block`/`ext_block` 中的每个执行方，填充 `resp->hp_proc` 并把请求原子地入队其 MPMC 队列；必要时对进程信号量执行 `sem_post`。
   * 客户端线程阻塞在 `req->sem_wakeup` 上等待应答。

3. **处理（服务端 / 工作线程）：**
   * 后台工作线程（绑定在自己的 CPU 核心上）在进程信号量上醒来。
   * 它从队列取出请求并发布 Hazard Pointer `thread->hp_req`，随后复查请求签名。
   * 从缓冲区反序列化参数，调用真正的 `log_write()` 实现。
   * 将返回值（若有）拷入自己的应答槽。

4. **完成：**
   * 工作线程将 `LIBSRPC_RC_FLAG_READY` 标志写入 `resp->rc`，对 `req->sem_wakeup` 执行 `sem_post`，并清除 Hazard Pointer。
   * 客户端醒来，重新统计已收到的应答，收齐后读取结果。其余结果可通过 `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()` 获取。
   * 请求块在 TLS 中保持缓存以供下次调用；释放（`libsrpc_shmem_free`）发生在块被更大的块替换时，或进程清理期间。

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

* **匿名内存（memfd）：** 使用 `memfd_create` 而非 `shm_open`，可以创建在文件系统（`/dev/shm`）中不可见的共享内存对象，提高安全性并简化资源清理：内存随描述符的最后一个引用一并消失。
* **固定基地址：** 映射在 `SHMEM_BASE_VADR`（默认 `0x200000000000`）处以 `MAP_FIXED_NOREPLACE` 完成，因为池内存放的是**绝对**指针。`NOREPLACE` 标志保证：与既有映射冲突时，`mmap` 会报错，而不是悄悄覆盖他人区域。
* **Fork 防护：** 对共享内存区域调用 `madvise(..., MADV_DONTFORK)`，防止子进程继承该内存并破坏 TLSF 分配器。
* **多核：** 工作线程按被允许的 CPU 数（`sched_getaffinity`）创建，并通过 `pthread_attr_setaffinity_np`（`pthread_start_all_cpu`）绑定到核心，从而最小化上下文切换与缓存迁移开销。
* **BUILD_TS 构建隔离：** 构建时间戳参与守护进程名、抽象 socket 名、共享内存签名以及注册消息签名。这实质上是一个自动生成的 ABI 版本：以不同 `libsrpc.so` 构建的应用彼此不可见。一个实际后果——应用不能脱离库单独重编。
* **构建期参数化：** 池大小（`SHMEM_SIZE_KB`）、基地址（`SHMEM_BASE_VADR`）、队列容量（`MPMCQ_DEGREE`）、调试消息级别（`DBG_LVL`）以及分配器替换开关（`DISABLE_ALLOC`）均由 CMake 变量设定，并以 `-D` 定义的形式进入代码。
* **严格构建标志：** 库与分配器均以 `-Wall -Wextra -Werror` 编译，因此强制显式标注未使用的参数（`__attribute__((unused))`）。

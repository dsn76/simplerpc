# simplerpc Architecture

This document describes the internal design, components, and interaction mechanisms of the `libsrpc` library.

## 🏗 General Concept

**simplerpc** is a transparent RPC framework that operates over shared memory. Unlike classic RPC systems that serialize into a byte stream (Protobuf, JSON) and transfer over a network, `simplerpc` passes call arguments in a common memory segment mapped by all processes at the same virtual address, which allows avoiding copies between address spaces.

### Key Principles
1. **Zero-Conf Daemon:** The background coordinator process is started automatically when the library is loaded into any application and exits after the last client disconnects.
2. **Memory-Centric:** All data, process descriptors, the function registry, and message queues reside in shared memory managed by the transactional TLSF allocator.
3. **Link-Time Dispatch:** The role of a process (caller or executor) is determined by the linker via `weak alias`, not by configuration or runtime registration. There is no IDL — the function list is C prototypes, and `srpc_rpcgen` emits the stubs.
4. **Ownership-Based Cleanup:** Every memory block is tagged with the UID of its owner process, so everything owned by a crashed or disconnected process can be reclaimed without keeping an external registry of allocations.
5. **Transparent IPC (optional):** Substituting the standard functions (`malloc`, `free`) lets existing code work with shared memory unmodified. Disabled by default via the build option `DISABLE_ALLOC=ON`.

## 🧩 Main Components

### 1. Guard Daemon (Background Daemon)
The coordinating process that manages the lifecycle of the memory pool, the function registry, and the process descriptors. RPC traffic does not go through the daemon — it is only the owner of resources and the "notary" of registration.

* **Startup:** Implemented by the `spawn_daemon()` function. The daemon is created with a double `fork()` and `setsid()`, which makes it an orphan with no controlling terminal and leaves no zombie behind for the caller.
* **Embedded Loader:** The daemon's executable file does not live on disk. It is embedded in the `libsrpc.so` library itself as a C array (`xxd -i -n loader` from `src/libsrpc_loader.c`), written to a temporary `memfd` in RAM, and started via `fexecve()`. The loader gets the path to the library through the `LIBSIMPLERPC_SO` environment variable (its value is obtained via `dladdr()`), calls `dlopen()`, and transfers control to `simplerpc_daemon_main()`.
* **Why a separate process instead of a thread:** the daemon needs a clean address space — it maps the pool at the fixed address `SHMEM_BASE_VADR` with `MAP_FIXED_NOREPLACE` and must not carry the parent application's allocations and threads.
* **Leader election:** *every* process tries to start a daemon, but `bind()` of the abstract Unix socket is atomic: losers get `EADDRINUSE` and exit silently. The client retries `connect()` on `ECONNREFUSED` (up to ~1 s).
* **Role:** creates the abstract Unix socket, allocates the shared memory, hands out the memory file descriptors to connecting clients, maintains the RPC function registry, and starts the garbage collector thread.
* **Exit:** the main loop is `epoll_wait(timeout = 1 s)`; as soon as the client counter drops to zero, the daemon exits and the shared memory (anonymous `memfd`) disappears with the last reference.

### 2. Transport Layer (Unix Sockets & SCM_RIGHTS)
Used exclusively for signaling, function registration, and connection establishment.

* **Abstract socket:** the socket name is formed as `"simplerpc_" BUILD_TS`, where `BUILD_TS` is the build timestamp. Applications linked against different builds of the library end up in different, isolated "domains".
* **Connection sequence (`libsrpc_unix_server_addclient`):** `accept4()` → `SO_PEERCRED` (learn the client PID) → `libsrpc_proc_create(pid)` → `SO_RCVTIMEO = 5 s` → read the registration message → register functions → pass the memory FD → add to `epoll`.
* **Client-Server:** only after successful registration does the daemon pass the client the shared-memory file descriptor (FD) via the `SCM_RIGHTS` mechanism (ancillary data). A client that does not pass the signature check is never given access to the pool.
* **mmap:** after receiving the FD, the client performs `mmap()` of the same memory region as the daemon, at the pre-agreed virtual address (`SHMEM_BASE_VADR`) with the `MAP_FIXED_NOREPLACE` flag. This guarantees that pointers created in one process are valid in another. Additionally, the pool header signature is verified.
* **Death detector:** a connection break (`EPOLLHUP | EPOLLRDHUP` or `recv() == 0`) is the only signal needed to know that the client is gone. The kernel closes the socket, so the event arrives on `exit()`, on `SIGKILL`, and on segfault alike.
* **Client control thread:** on the application side, the connection is served by a separate detached `unix_client` thread that performs all initialization (registration, `mmap`, starting the executors) and then sits in `epoll_wait`. Wake-up for shutdown is via `eventfd`.

### 3. Memory Management (TLSF Shared Memory)

* **tlsf_txn v4.1:** a transactional TLSF allocator whose sources are included in the project tree (`libs/tlsf_txn/`, no git submodules). It provides deterministic O(1) block allocation thanks to the `FL × SL` matrix of free lists and bitmaps. The pool format is versioned (`TLSF_FORMAT_VERSION`); foreign formats are rejected.
* **Robust Mutex inside the allocator:** mutual exclusion between processes is provided by a `pthread_mutex_t` with attributes `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST | PTHREAD_MUTEX_RECURSIVE`, placed *inside* the pool's control structure. `libsrpc` itself never takes it — it simply calls `tlsf_*` (unlike version 0.1.x, where the mutex lived in the libsrpc structure and was locked manually around every operation).
* **BUL (Binary Undo Log):** before every 8-byte metadata word is written, the old value is saved on an undo stack inside the pool. A successful operation commits (the stack top is reset). If the mutex owner dies, the next `tlsf_lock()` receives `EOWNERDEAD`, automatically rolls back the unfinished transaction (`bul_recover`), and calls `pthread_mutex_consistent()`. The pool stays consistent without an external watchdog.
* **Ownership Model (UID):** each block header contains a `uid[12]` array. A block is returned to the pool only when all its owners have detached it. `libsrpc` passes the allocator the `proc_uid` — a 16-bit process identifier issued by the daemon (`DAEMON_PROC_UID = 1` for the daemon itself). The `libsrpc_shmem_link()` function lets a process add itself as an owner of a foreign block, protecting it from being freed.
* **Block Typing:** a 15-bit `data_type` field in the block header holds one of the `libsrpc_shm_data_type_t` types: `USER` (application data), `PROC` (process descriptor), `REGFN` (function registry block), `REQUEST` (RPC request block). The garbage collector and destructors use this field to tell user memory apart from service memory.
* **Deferred-Cleanup Flag (`dc`):** 1 bit in the same header field. It serves as a retire mark: "the block is logically dead, but it cannot be freed right now". Set via `libsrpc_shmem_free_dc()` (= `tlsf_setdc()`), read by the garbage collector.
* **Allocator Hijacking (optional):** the library can intercept the standard functions `malloc`, `calloc`, `realloc`, and `free`, obtaining the originals via `dlsym(RTLD_NEXT)`. Using `Thread Local Storage` (TLS), a thread switches between the system allocator and the shared-memory allocator "on the fly" (`libsrpc_alloc_sw_std()` / `libsrpc_alloc_sw_shm()`). By default this whole code path is excluded from the build via `DISABLE_ALLOC=ON`.

### 4. Garbage Collector (Shared Memory GC)
A separate thread living **only in the daemon** (initialized from `libsrpc_shmem_create()`) is responsible for reclaiming service structures that cannot be freed immediately because of possible readers in other processes.

* **Alarm in shared memory:** the GC structure (thread + semaphore) lives in `libsrpc_shmem_t`, so `libsrpc_shm_gc_wakeup()` can wake the collector from any process via `sem_post`. In addition, the GC wakes up on a timeout, `SHM_GC_FREQ_CHECK` times per second (10 by default).
* **Garbage search:** `tlsf_free_uid_blocks(pool, 0, ...)` — a physical walk of the heap looking for allocated blocks with the `dc == 1` flag. There is no separate list of deferred objects: the header flag plays that role.
* **Occupancy check (Hazard Pointers):** the found blocks are collected into a local accumulator pool, then `libsrpc_req_is_busy_proc()` is called for each request, which walks all live `libsrpc_proc_t` and all their `threads[i].hp_req`. If at least one executor thread has published a pointer to this request, the block stays until the next pass.
* **Batched freeing:** free blocks are removed in a single batch under one acquisition of `tlsf_lock()` / `tlsf_free_nb()` × N / `tlsf_unlock()` — to avoid grabbing the cross-process mutex for every block.
* **Cleanup after a crashed process:** on connection break the daemon calls `libsrpc_proc_destroy()`, which removes the process from the function registry, invalidates its descriptor, cancels pending requests (`-ECANCELLED`), and calls `tlsf_free_uid_blocks(pool, proc_uid, ...)` — a direct heap walk by owner UID. User blocks are freed immediately; service blocks are marked `dc` and handed to the GC.

### 5. RPC Mechanism and Stub Generation
Boilerplate (prototypes, identifiers, wrappers, the table, the dispatcher's `switch`) is generated by `tools/srpc_rpcgen.c` from `src/libsrpc_rpc_functions.txt` into a set of `*.inl` files included from `libsrpc.h` / `libsrpc_private.h` / `libsrpc.c`.

* **Function list:** C prototypes. Format: `rettype name(types...) [KEY=VALUE ...];`. The default policy is `RPC_SEND_ALL`; otherwise `RPC_MODE=RPC_SEND_*` after `)`. User-defined types come from `#include` in the same file.
* **Identifiers:** each function gets a unique ID `sRPCFNID_<name>` (numbering starts at `sRPCFNID_START = 16`) and a registry index `sRPC_ID2IDX(id)`.
* **Wrapper generation:** `srpc_rpcgen` writes stub functions `librpcimp_<name>`. They serialize the arguments into the request buffer (`libsrpc_request_t`) and place it into the executor's MPMC queue.
* **Dispatch:** on the executor side, the generated `switch` deserializes the buffer into stack variables, calls the original function, and copies the result into the response slot. The generator emits different code for `void` and non-`void`.
* **Design limitations:** functions with a variable number of arguments are not allowed; arguments are copied byte by byte (`memcpy`), so only scalar types and pointers are permitted, and a pointer is meaningful only if it addresses the shared pool.

### 6. Dynamic Linking of RPC Functions
Registration of "who can execute what" goes through four stages and requires neither `dlsym` nor runtime stub generation.

1. **Weak alias (linking stage).** For every function in the list the library declares `__attribute__((weak, alias("librpcimp_" #name))) rettype name(...);`. If the application defines the function itself — the strong symbol overrides the alias, and the process becomes the **executor**. If not — the symbol resolves to the stub, and the process becomes the **caller**. The decision is made independently for each function: one process can be the executor of some functions and the caller of others.
2. **Bitmap (process start).** The table `srpc_fn[]` stores, for each function, a pair of addresses: `.rpc` (the stub's address) and `.loc` (the address the symbol actually resolved to). The `libsrpc_bmp_func_set()` function compares them and sets the bit in `srpc_bmp_func_t` — "I can execute this". A comparison of two pointers, no strings and no runtime resolution.
3. **Passing to the daemon (connection).** The bitmap is sent to the daemon over the Unix socket in a `regfn_msg_t { size, sign[32], bmp }` message right after `connect`, before receiving the memory FD. The daemon checks the size and the signature (`"simplerpc_" BUILD_TS`) — this protects against applications with different versions of the RPC function list connecting to each other.
4. **Registry entry (daemon).** `libsrpc_reg_func_form_bmp()` inserts the client's `libsrpc_proc_t` pointer into the `srpc_regfn_shm_t` registry, which lives in shared memory. The client itself never writes to the registry.

**Registry layout** (`srpc_regfn_shm_t`, one entry per function):

* `main_block` — 8 slots of `_Atomic(libsrpc_proc_t *)` right in the structure, without any allocation. The typical case (a handful of executors per function) requires zero memory allocations.
* `ext_block` — when the main block overflows, an extension of `+32` slots is allocated, the old contents are copied, and the new block is published via `atomic_compare_exchange_strong`. The race loser frees its block and retries.
* The old `ext_block` is not freed immediately: its signature is cleared, the block is marked `dc` (`libsrpc_shmem_free_dc`) and the GC is woken — an RCU-like scheme of deferred reclamation.
* `num_all` — an atomic counter of the function's registered executors. The client reads it when preparing a request to know how many responses to reserve. If `num_all == 0`, the call fails immediately with `-ENOREGFUN`.
* Slot insertion and removal are done with CAS operations, without a single mutex.

**Deregistration** happens on socket close: `libsrpc_proc_destroy()` → `libsrpc_unreg_func_proc()` removes the process from all `main_block`/`ext_block` and decrements `num_all`.

**Linking note:** an application that *only* exports RPC functions may have no references to any `libsrpc.so` symbols. With the default linker flag (`--as-needed`), the `DT_NEEDED` entry is dropped, the library's constructor never runs, and the process does not even connect to the pool. That is why `-Wl,--no-as-needed` is mandatory in `example/` and `bench/`.

### 7. Process and Thread Descriptors
The `libsrpc_proc_t` structure is created by the daemon in shared memory for every connected client (block type `LIBSRPC_SHMDT_PROC`) and is the entry point for all requests addressed to it.

* **Signature and status:** `sign` (`LIBSRPC_PROC_SIGN`) and `status` are atomic. The `libsrpc_proc_is_valid()` function requires a correct signature and the `RUN` status, so invalidating a process is instantly visible to everyone else, without locks.
* **`proc_uid`:** a 16-bit identifier issued by the daemon; used as the owner UID in the TLSF allocator.
* **Queue and semaphore:** each process has its own MPMC queue and a single wake-up semaphore `sem_wakeup` for all its executor threads.
* **Thread array:** `threads[]` is placed in the same memory block, sized by the number of online CPUs (`sysconf(_SC_NPROCESSORS_ONLN)`). Workers themselves are started according to the `sched_getaffinity` mask (`pthread_start_all_cpu`). Each element contains `hp_req` — a Hazard Pointer to the request being processed.
* **Request list:** `req_head` — the list of all the process's request blocks, for the garbage collector's walk.

### 8. Synchronization and Queues
The custom futex implementation (`libsrpc_futex.*`) that was present in version 0.1.x has been removed; synchronization is built on POSIX primitives in shared memory.

* **MPMC Queue:** the lock-free `lf_mpmc_queue` (Vyukov scheme: an array of cells with a `seq` counter), placed in shared memory inside `libsrpc_proc_t`. Used to hand tasks from the client to the executor's workers. Cells are cache-line aligned (`alignas(64)`), and `head` and `tail` are separated onto different lines. The capacity is a power of two, set at build time (`MPMCQ_DEGREE`, default `2^10 = 1024`). The cell holds only a pointer to the `libsrpc_request_t` in shared memory; the worker finds its response slot via `resp->hp_proc`.
* **POSIX Semaphores:** `sem_t` semaphores initialized with `pshared = 1` and placed in shared memory are used to sleep and wake worker threads when the queues have no tasks (CPU savings). They are wrapped by a thin `libsrpc_sem_*` layer (`libsrpc_wrapper.h`) so the implementation can be replaced if needed. Wake-ups are economical: `sem_post` is called if there are waiting threads (`threads_wait > 0`) or the process has exactly one worker (`threads_run == 1`).
* **Semaphore in the request body:** every `libsrpc_request_t` contains its own `sem_wakeup` — the executor uses it to notify the calling thread of a ready response. This replaced the "Job Futex" of earlier versions.
* **Spinlock list:** `libsrpc_list_spin` — a singly linked list, writes under a `pthread_spinlock_t`, lock-free traversal via atomic loads with `acquire`. Used for the process list and the request list.
* **Fixed-size block pool:** `libsrpc_fixblockalloc` — a fast pool on a FIFO index ring with automatic growth (`nextpool`), used by the daemon for client-connection descriptors in *local* (not shared) memory. It has no synchronization and is designed for single-threaded use — accesses come only from the daemon's main `epoll` loop.

### 9. Request Structure and Response Protocol
A single block in shared memory contains both the request and all its responses, so no extra allocation is needed for the answers.

```text
libsrpc_request_t
+----------------------------------------------------------------------+
| node | sem_wakeup | hp_regfn | sign | seq_num | funid | bufsz |      |
| retoff | retsz | retnum |                                            |
+----------------------------------------------------------------------+
| buf[]:                                                               |
|   [arguments .......]  <- retoff = ALIGNLONG(len(args))               |
|   [libsrpc_response_t #0][ret]  <- each of size retsz                 |
|   [libsrpc_response_t #1][ret]                                        |
|   ...                          <- retnum slots                        |
+----------------------------------------------------------------------+

libsrpc_response_t: { hp_proc (who we wait for), rc, buf[] }
```

* **Request block caching:** the pointer to the last block is kept in TLS (`current_req`). If the new request fits — the block is reused without touching the allocator; if not — it is freed and a new one is allocated at twice the size. On the hot path the allocator is never called at all. The same pointer serves `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()`.
* **Readiness protocol:** the executor first sets `LIBSRPC_RC_FLAG_LOCK` (`0xC0000000`), copies the result, and publishes `LIBSRPC_RC_FLAG_READY` (`0x80000000`), or writes a negative error code. In the wrapper, the client considers a response valid only on an exact match with the ready flag.
* **Validation:** `libsrpc_req_is_corrupted()` checks the signature (double-read with `acquire` before and after reading the fields) and the consistency of the sizes. `libsrpc_req_destroy()` clears the signature via CAS, so a double destroy is safe — this protects against use-after-free when the GC and an executor work concurrently.
* **Dispatch policy:** `RPC_SEND_ALL` — the request goes to all registered executors (`retnum = num_all`); `RPC_SEND_FIRST` — to the first available one (`retnum = 1`); `RPC_SEND_LAST` — to the last one in the `main_block` / `ext_block` walk; `RPC_SEND_RR` — to one executor in round-robin (atomic counter `req_send_rr`).
* **Partial success:** if fewer requests were dispatched than there are executors, the expected number of responses is lowered; if none at all — the caller gets `-ENOTAVAILABLE`.
* **Timeout:** waiting for responses is a `libsrpc_sem_wait_timeout_us` loop that re-counts ready slots after every wake-up. The interval is set in microseconds: `libsrpc_timeout_oneshot_set()` (the next call), `libsrpc_timeout_func_set()` (a function), `libsrpc_timeout_global_set()` (all calls); the default is `LIBSRPC_TIMEOUT_DEFAULT` (1 s), the lower bound is `LIBSRPC_TIMEOUT_MINIMUM` (100 µs). If the responses are not collected within the interval, the empty slots are marked `-ERPCWAITIMEDOUT`.

### 10. Recursion Protection and Error Handling
* **TLS flag:** executor threads have `srpc_disable_rpc_recursion` set, so an executor cannot initiate an RPC call itself — an attempt returns `-ERECURSIVE`.
* **Check in the dispatcher:** before the call, the addresses `&name` and `&sRPCFN(name)` are compared; if they match, the process ended up in the registry without a local implementation and the stub would call itself.
* **Error codes:** `libsrpc` extends the system `errno` with its own codes starting after `MAX_ERRNO`: `ELIBNOINIT`, `ESHMNOINIT`, `ERPCDISABLE`, `ENOREGFUN`, `ERECURSIVE`, `ECANCELLED`, `ENOTAVAILABLE`, `ERPCWAITIMEDOUT`. They are stored in a TLS variable, available via `libsrpc_errno_get()` (needed for `void` functions that cannot return a code) and decoded by `libsrpc_strerror()`.

## 🔄 Lifecycle of an RPC Call

1. **Initialization:**
   * The application loads `libsrpc.so`; a constructor with priority 101 runs.
   * The constructor obtains the original `malloc`/`free` via `dlsym(RTLD_NEXT)` and checks, by `program_invocation_name`, whether the current process is the Daemon. If not — it forks the Daemon (the daemon that loses the `bind()` race exits silently).
   * The control thread `unix_client` is started: it connects to the Daemon's Unix socket, sends the bitmap of its locally implemented functions, receives the memory FD, and does `mmap` at the fixed address.
   * The client picks up its `libsrpc_proc_t` and `proc_uid`, starts one worker thread per available CPU core, sets the `RUN` status, and enables RPC (`rpc_enable`).

2. **Function call (Client Side):**
   * The user calls `log_write("Hello")`; the symbol resolves to the generated wrapper `librpcimp_log_write`.
   * The wrapper reads `num_all` from the registry — if there are no executors, it fails immediately with `-ENOREGFUN`.
   * A `libsrpc_request_t` is allocated in Shared Memory (or the cached one is reused) via `libsrpc_shmem_malloc_type(..., LIBSRPC_SHMDT_REQUEST)`.
   * The arguments are copied into `req->buf`, and the response slots are laid out.
   * For each executor from `main_block`/`ext_block`, `resp->hp_proc` is filled and the request is atomically enqueued into its MPMC queue; a `sem_post` on the process semaphore is done when needed.
   * The client thread blocks on `req->sem_wakeup`, waiting for the responses.

3. **Processing (Server Side / Worker Thread):**
   * The background worker thread (pinned to its CPU core) wakes up on the process semaphore.
   * It dequeues the request and publishes the Hazard Pointer `thread->hp_req`, after which it re-checks the request signature.
   * It deserializes the arguments from the buffer and calls the real `log_write()` implementation.
   * It copies the return value (if any) into its response slot.

4. **Completion:**
   * The worker writes the `LIBSRPC_RC_FLAG_READY` flag into `resp->rc`, does `sem_post` on `req->sem_wakeup`, and removes the Hazard Pointer.
   * The client wakes up, re-counts the received responses and, having gathered all of them, reads the result. The remaining results are available via `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()`.
   * The request block remains cached in TLS for the next call; freeing (`libsrpc_shmem_free`) happens when the block is replaced by a bigger one or during the process cleanup.

## 📊 Interaction Diagram

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

## ⚙ Implementation Details

* **Anonymous memory (memfd):** instead of `shm_open`, `memfd_create` is used, which allows creating shared-memory objects that are not visible in the file system (`/dev/shm`), improving security and simplifying resource cleanup: the memory disappears with the last reference to the descriptor.
* **Fixed base address:** the mapping is done with `MAP_FIXED_NOREPLACE` at the address `SHMEM_BASE_VADR` (default `0x200000000000`), because the pool holds **absolute** pointers inside. The `NOREPLACE` flag guarantees that on a conflict with an existing mapping, `mmap` fails with an error instead of silently overwriting someone else's region.
* **Fork protection:** `madvise(..., MADV_DONTFORK)` is called for the shared-memory region to prevent the memory from being inherited by child processes and breaking the TLSF allocator.
* **Multicore:** worker threads are created by the number of allowed CPUs (`sched_getaffinity`) and pinned to cores via `pthread_attr_setaffinity_np` (`pthread_start_all_cpu`), which minimizes context-switch and cache-migration overhead.
* **Build isolation via BUILD_TS:** the build timestamp is part of the daemon name, the abstract socket name, the shared-memory signature, and the registration message signature. This is essentially an auto-generated ABI version: applications built with different `libsrpc.so` builds do not see each other. A practical consequence — an application cannot be rebuilt separately from the library.
* **Build-time parameterization:** the pool size (`SHMEM_SIZE_KB`), the base address (`SHMEM_BASE_VADR`), the queue capacity (`MPMCQ_DEGREE`), the debug message level (`DBG_LVL`), and the allocator-substitution switch (`DISABLE_ALLOC`) are set by CMake variables and end up in the code as `-D` definitions.
* **Strict build flags:** both the library and the allocator are compiled with `-Wall -Wextra -Werror`, which forces unused parameters to be explicitly marked (`__attribute__((unused))`).

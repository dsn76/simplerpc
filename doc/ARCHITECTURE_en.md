# SimpleRPC Architecture

This document describes the internal design of `libsrpc`: what the library is made of, how a call proceeds, what happens when processes die, and where the current implementation is weak. The user-facing side — build, API, examples — is described in the [README](../README_en.md).

## 🏗 General Concept

**simplerpc** is RPC on top of shared memory. Classical RPC systems serialize a call into a byte stream (Protobuf, JSON) and push it through a socket. `simplerpc` places the arguments in a common pool that every process maps at the same virtual address. A pointer obtained in one process is valid in another, and the socket is needed only for housekeeping messages.

### Key Principles

1. **Zero-Conf Daemon.** A background coordinator process starts itself when the first process loads the library and exits after the last client disconnects. There is no configuration, no PID file, and no service.
2. **Memory-Centric.** Process descriptors, the function registry, queues, requests, and responses live in the shared pool under the transactional TLSF allocator. Call data does not travel through the socket.
3. **Link-Time Dispatch.** The linker chooses a process's role for each function through a `weak alias`: defined the function — executor, did not define it — caller. There is no IDL and no code generator; everything is expanded by the preprocessor from a single X-macro.
4. **Ownership-Based Cleanup.** Every pool block is tagged with the UIDs of its owner processes. A dead process is cleaned up by this UID, with no separate registry of allocations. A live request can defer the cleanup of a particular process's user blocks by holding its UID.
5. **Transparent IPC (optional).** Substituting `malloc`/`free` lets existing code work with the pool unchanged. Disabled by default (`DISABLE_ALLOC=ON`).

## 🧩 Components

### 1. Coordinator Daemon (Guard Daemon)

The daemon owns the pool, the function registry, and the process descriptors. RPC traffic does not pass through it: the daemon only hands out memory, registers executors, and cleans up after the dead.

* **Startup.** `spawn_daemon()` does a double `fork()` with `setsid()` in between. The first child exits immediately, and the parent reaps it with `waitpid()`. The second becomes an orphan with no controlling terminal and leaves no zombie.
* **Embedded loader.** The daemon executable does not exist on disk. At build time `src/libsrpc_loader.c` is compiled with `gcc` into a small dynamically linked ELF with no symbols. `xxd -i -n loader` turns it into a C array, and the array is compiled into `libsrpc.so`. At startup the array is written to a `memfd` and executed via `fexecve()`. `argv[0]` receives the daemon name `"simplerpc_" BUILD_TS`. The environment consists of a single variable, `LIBSIMPLERPC_SO`, holding the path to the library obtained through `dladdr()`. The loader `dlopen()`s that library and transfers control to `simplerpc_daemon_main()`.
* **Why a process and not a thread.** The daemon needs a clean address space: it maps the pool at the fixed address `SHMEM_BASE_VADR` and must not carry the allocations, threads, and descriptors of the application that spawned it. Its lifetime must also be independent of that application's.
* **Leader election.** Every process tries to start the daemon, but `bind()` of the abstract socket is atomic. The losers get `EADDRINUSE` and exit silently. The client retries `connect()` on `ECONNREFUSED` up to 10,000 times with a 100 µs pause, about one second in total.
* **Duties.** Listen on the abstract socket, create the pool and hand out its descriptor, maintain the function registry, start the garbage-collector thread, clean up after disconnected clients.
* **Shutdown.** The main loop is `epoll_wait()` with a 1 s timeout. When the client counter drops to zero, the daemon exits, and the pool's anonymous `memfd` disappears with the last reference.
* **Output.** The daemon inherits `stdout` and `stderr` from the process that started it, so the daemon's messages appear in that process's terminal.

### 2. Transport (UNIX Socket and SCM_RIGHTS)

The socket serves only for connecting, registering functions, passing the pool descriptor, and detecting a client's death.

* **Abstract socket.** The name is `"simplerpc_" BUILD_TS`, where `BUILD_TS` is the timestamp of the build configuration. Processes with different builds of `libsrpc.so` land in different, non-overlapping domains. An abstract socket has no filesystem permissions.
* **Connection order** (`libsrpc_unix_server_addclient`): `accept4()` → `SO_PEERCRED` (only the PID is taken from the credentials) → `libsrpc_proc_create(pid)` → `SO_RCVTIMEO = 5 s` → reading the registration message → registering functions → passing the pool FD → adding to `epoll`. A client with a wrong signature is not admitted to the pool. The client's UID is not checked.
* **Mapping the pool.** Having received the FD, the client `mmap()`s it with `MAP_FIXED_NOREPLACE` at `SHMEM_BASE_VADR` and checks the signature in the pool header. If the address is taken, `mmap()` fails instead of overwriting someone else's mapping.
* **Death detector.** A connection break (`EPOLLHUP | EPOLLRDHUP` or `recv() == 0`) is the single sufficient sign that the client is gone. The kernel closes the socket, so the event arrives the same way for `exit()`, `SIGKILL`, and a segfault. No heartbeat and no timeouts are needed for this.
* **Client control thread.** In the application the connection is served by a detached `unix_client` thread. It connects, sends the registration, receives the FD, maps the pool, finds its process descriptor, starts the executor threads, sets the status to `RUN`, and enables RPC (`rpc_enable`). The library constructor waits on a semaphore until the thread finishes initialization or reports an error, so by the time `main()` is entered the process is ready. The thread then sleeps in `epoll_wait()`. It is woken for exit through an `eventfd`.

### 3. Memory Management (TLSF in Shared Memory)

* **tlsf_txn v4.1.** A transactional TLSF allocator whose sources live in the project tree (`libs/tlsf_txn/`). Allocation is O(1) thanks to the `FL × SL` matrix of free lists and bitmaps. The pool format is versioned (`TLSF_FORMAT_VERSION = 0x00040000`); a foreign format is rejected.
* **Robust mutex inside the pool.** Cross-process exclusion is provided by a `pthread_mutex_t` with attributes `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST | PTHREAD_MUTEX_RECURSIVE`, placed in the pool's control structure. `libsrpc` never takes it itself and only calls `tlsf_*`. The exception is the garbage collector, which frees a batch of blocks under one explicit `tlsf_lock()`.
* **BUL (Binary Undo Log).** Before every 8-byte metadata word is written, the old value is pushed onto an undo stack inside the pool; a successful operation resets the stack (commit). If the mutex owner dies, the next acquisition receives `EOWNERDEAD`, rolls the unfinished operation back, and calls `pthread_mutex_consistent()`. The pool stays consistent without an external watchdog. Only the allocator's metadata is rolled back: the journal does not protect the data inside blocks.
* **Owners (UID).** The block header holds a `uid[12]` array. A block returns to the pool only when every owner has been removed from the array. `libsrpc` passes the allocator a `proc_uid` — a 16-bit identifier the daemon issues to each client (the daemon itself has `DAEMON_PROC_UID = 1`). Identifiers are reused after processes disconnect.
  * `libsrpc_shmem_link()` adds an owner.
  * `libsrpc_shmem_free()` removes only the caller's UID.
  * `libsrpc_shmem_realloc()` works only for a block with a single owner.
  * `libsrpc_shmem_get_size()` reads the payload size.
* **Block types.** A 15-bit `data_type` field in the header stores a `libsrpc_shm_data_type_t`: `USER` (application data), `PROC` (process descriptor), `REGFN` (function-registry block), `REQUEST` (request block). Destructors and the garbage collector use it to tell user memory from service memory.
* **Deferred-cleanup flag (`dc`).** One bit in the same word. A mark meaning "the block is logically dead, but it cannot be freed right now". Set via `libsrpc_shmem_free_dc()` (= `tlsf_setdc()`), read by the garbage collector.
* **Error codes.** Memory functions write an allocator code `TLSF_ERR_*` (1…15) into the thread's error code (`libsrpc_errno_get()`). These numbers overlap the system `errno`, so `tlsf_strerror()` decodes them, not `libsrpc_strerror()`.
* **Allocator substitution (optional).** The library can intercept `malloc`, `calloc`, `realloc`, and `free`, obtaining the originals via `dlsym(RTLD_NEXT)`. A flag in TLS switches a thread between the system heap and the pool (`libsrpc_alloc_sw_std()` / `libsrpc_alloc_sw_shm()`). The interception code is not built by default (`DISABLE_ALLOC=ON`).

### 4. Garbage Collector (Shared Memory GC)

A separate thread living **only in the daemon** (started from `libsrpc_shmem_create()`). It frees service blocks that could not be freed immediately because threads of other processes might still have been reading them, and user blocks whose cleanup was deferred by a hold.

* **Alarm in the pool.** The GC structure — thread, semaphore, and the deferred-block list `list_head` — lives in `libsrpc_shmem_t`, so `libsrpc_shm_gc_wakeup()` wakes the collector from any process via `sem_post`. In addition the GC wakes on its own `SHM_GC_FREQ_CHECK` times per second (10 by default).
* **Service blocks.** `tlsf_free_uid_blocks(pool, 0, ...)` walks the heap and collects allocated blocks with `dc == 1` into a local accumulator (up to 1024 per pass). Service blocks have no separate list: the header flag plays that role.
* **Occupancy check (Hazard Pointers).** For every found request, `libsrpc_req_is_busy_proc()` checks `threads[i].hp_req` of all live processes. If at least one executor thread has published a pointer to this request, the block stays until the next pass.
* **Deferred user blocks.** If a process dies while its UID is held, pointers to its user blocks go into a `gc.list_head` node together with the UID. The nodes are allocated with ordinary `malloc` in the daemon's address space and are not part of the pool. On every pass the GC checks whether anyone still holds that UID. If not, the node's blocks are freed.
* **Batched freeing.** Blocks ready to be freed are removed in one batch: `tlsf_lock()`, `tlsf_free_nb()` × N, `tlsf_unlock()`. The cross-process mutex is thus taken once per pass, not once per block.

### 5. RPC Mechanism and X-Macros

The boilerplate is generated by the preprocessor (`src/macro.h`, `src/argfunc.h`).

* **RPC_LIST.** The X-macro in `src/libsrpc_rpc_functions.h` is the single source of truth. An entry: `XF(dispatch policy, return type, name, parameter types...)`. From it are expanded prototypes, identifiers, wrappers, the function table, and the dispatcher's `switch`.
* **Identifiers.** Inside the library a function gets `sRPCFNID_<name>` (numbering starts after `sRPCFNID_START = 16`) and a registry index `sRPC_ID2IDX(id)`. In the public `libsrpc.h` the same number is visible as `libsrpc_funid_<name>`.
* **Wrappers.** Each function gets a generated stub `librpcimp_<name>` (`sRPCFN(name)`). It serializes the arguments into a `libsrpc_request_t` request block, dispatches it to the executors' queues, waits for the replies, and returns the result of the first slot.
* **Dispatch.** On the executor side the macros `M_DECL`, `M_EXTRACT`, and `M_ARGNAMES` unpack the buffer back into stack variables, call the real function, and write the result into the response slot. For `void` functions the slot receives the ready flag with no data.
* **Branching on the return type.** The C preprocessor cannot compare types, so a macro `COMPARE_void`, defined only for `void`, is used. `IIF(EQUAL(rettype, void))` expands into different code for `void` and non-`void` functions (`RETDATA`).
* **Built-in limitations.** Functions with a variable number of arguments are not allowed. Arguments are copied byte by byte (`memcpy`), so scalar types and pointers are passed, and a pointer is meaningful only if it addresses the common pool.

### 6. Function Registration

Registration of "who can execute what" goes through four stages and requires neither `dlsym` nor runtime stub generation.

1. **Weak alias (linking).** For every function in `RPC_LIST` the library declares `__attribute__((weak, alias("librpcimp_" #name))) rettype name(...);`. If the application defined the function, the strong symbol overrides the alias and the process becomes the **executor**. If not, the symbol resolves to the stub and the process becomes the **caller**. The decision is made per function.
2. **Bitmap (process start).** The table `srpc_fn[]` stores, for each function, a pair of addresses: `.rpc` (the stub) and `.loc` (where the symbol actually resolved). `libsrpc_bmp_func_set()` compares them and sets the "I can execute this" bit in `srpc_bmp_func_t`. This is a comparison of two pointers, with no strings and no symbol lookup.
3. **Passing to the daemon (connection).** The bitmap goes to the daemon in a `regfn_msg_t { size, sign[32], bmp }` message right after `connect()`, before the pool FD is received. The daemon checks the size and the signature `"simplerpc_" BUILD_TS`: this protects against processes with different `RPC_LIST`s connecting.
4. **Writing the registry (daemon).** `libsrpc_reg_func_form_bmp()` inserts a pointer to the client's `libsrpc_proc_t` into the `srpc_regfn_shm_t` registry in the pool. Clients never write to the registry.

**Registry layout** (one entry per function):

* `main_block` — 8 slots of `_Atomic(libsrpc_proc_t *)` right in the structure. The typical case, a handful of executors per function, needs no allocation.
* `ext_block` — when the main block overflows, an extension 32 slots larger than the previous one is allocated, the old contents are copied, and the new block is published via CAS. The registry has a single writer, the daemon's main thread; the CAS guards the publication.
* The old `ext_block` is not freed immediately: its signature is cleared, the block is marked `dc`, and the GC is woken. This is an RCU-like deferred reclamation, because readers in other processes may still be walking the old block.
* `num_all` — an atomic counter of the function's executors. The caller reads it to reserve response slots. The same value is exposed as `libsrpc_fnreg_num_get()`. When `num_all == 0` the call fails immediately with `ENOREGFUN`.
* Slots are inserted and removed with CAS operations, without mutexes.

**Deregistration** happens when the socket closes: `libsrpc_proc_destroy()` → `libsrpc_unreg_func_proc()` removes the process from every registry block and decrements `num_all`.

**Linking note.** An application that only executes functions may reference no symbol of `libsrpc.so`. With the linker's `--as-needed` flag the `DT_NEEDED` dependency is dropped, the library constructor never runs, and the process never connects to the pool. That is why `example/` and `bench/` use `-Wl,--no-as-needed`.

### 7. Process and Thread Descriptors

The `libsrpc_proc_t` structure is created by the daemon in the pool for every connected client (block type `PROC`). It is the entry point for all requests addressed to it.

* **Signature and status.** `sign` (`LIBSRPC_PROC_SIGN`) and `status` are atomic. `libsrpc_proc_is_valid()` requires a correct signature and the `RUN` status, so invalidating a process is visible to the others immediately and without locks.
* **`proc_uid`.** A 16-bit identifier from the daemon, also the owner UID in the allocator.
* **Queue and semaphore.** A process has one MPMC queue of incoming requests and one wake-up semaphore `sem_wakeup` for all its executor threads.
* **Executor threads.** The `threads[]` array lies in the same block; its size is the number of online CPUs (`sysconf(_SC_NPROCESSORS_ONLN)`). Threads are started according to the `sched_getaffinity` mask, one per available core, and pinned to it (`pthread_start_all_cpu`). **Every** process that loaded the library gets these threads, including caller-only ones. An array element holds `hp_req` — a Hazard Pointer to the request being processed.
* **Request list.** `req_head` is the list of all the process's request blocks. The garbage collector walks it looking for busy requests, and process cleanup checks UID holds on it.

### 8. Synchronization and Queues

The custom futexes (`libsrpc_futex.*`) from version 0.1.x have been removed; synchronization is built on POSIX primitives in the pool.

* **MPMC queue.** `lf_mpmc_queue` is a lock-free queue after Vyukov's scheme (an array of cells with a `seq` counter) inside `libsrpc_proc_t`. Cells are cache-line aligned (`alignas(64)`), and `head` and `tail` sit on different lines. The capacity is a power of two, set at build time (`MPMCQ_DEGREE`, `2^10 = 1024` by default). A cell holds only a pointer to the request, and the executor finds its response slot by `resp->hp_proc`.
* **Enqueue.** If the queue is full, the caller retries: 16 rounds of 32 attempts with a `pause` instruction, and `sched_yield()` between rounds. If no room appears, the response slot receives `-ENOMEM` and the request is not sent to that executor.
* **Executor waiting.** After draining the queue, an executor thread does not sleep immediately: for about 1024 iterations it actively polls the queue with pauses (`DEQUEUE_RETRY`, `libsrpc_cpu_pause(32)`). Only then does it mark itself in `threads_wait`, check the queue once more, and sleep on `sem_wakeup`. The short busy wait saves a wake-up under a dense stream of requests, but it costs CPU time after every request.
* **Wake-up.** The caller does `sem_post` on the process semaphore only when there are sleeping threads (`threads_wait > 0`). The `sem_t` semaphores are created with `pshared = 1` and wrapped by a thin `libsrpc_sem_*` layer (`libsrpc_wrapper.h`).
* **Semaphore in the request.** Every `libsrpc_request_t` has its own `sem_wakeup`: the executor wakes the calling thread on it when the reply is ready.
* **Spinlock list.** `libsrpc_list_spin` is a singly linked list. Writes go under a `pthread_spinlock_t`, traversal is lock-free through atomic loads with `acquire`. Used for the process list, the request list, and the GC's deferred-block list.
* **Fixed-size block pool.** `libsrpc_fixblockalloc` is a pool on a FIFO index ring with growth (`nextpool`). The daemon keeps client-connection descriptors in it, in **local** memory. The pool has no synchronization: only the daemon's main `epoll` loop touches it.

### 9. Request and Response

A single pool block holds both the request and all its responses, so no memory is allocated separately for the replies.

```text
libsrpc_request_t
+----------------------------------------------------------------------+
| node | sem_wakeup | hp_regfn | sign | seq_num | funid | bufsz |      |
| retoff | retsz | retnum | gc_lock_uid |                              |
+----------------------------------------------------------------------+
| buf[]:                                                               |
|   [arguments .......]  <- retoff = ALIGNLONG(len(args))               |
|   [libsrpc_response_t #0][ret]  <- each of size retsz                 |
|   [libsrpc_response_t #1][ret]                                        |
|   ...                          <- retnum slots                        |
+----------------------------------------------------------------------+

libsrpc_response_t: { hp_proc (who we wait for), rc, buf[] }
```

* **Request block cache.** The pointer to the thread's last block is kept in TLS (`current_req`). If the new request fits, the block is reused without touching the allocator. If not, a new one is allocated at twice the size. The old block is freed immediately, or, if it still holds an unfinished or timed-out reply, through the `dc` flag and the garbage collector. On the hot path the allocator is never called. The same pointer serves `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()`. The block is registered as thread data (`pthread_key_create`) and freed when the thread exits.
* **UID hold (`gc_lock_uid`).** Four 16-bit slots in the request body. `libsrpc_shmem_proc_lock(idx, ptr)` takes the executor from response slot `idx`. If `tlsf_check_uid()` confirms that `ptr` belongs to its `proc_uid`, the UID is written into a free slot and the function returns the slot number (`0…3`). When the cached block is replaced the mask is copied into the new one; reuse does not clear it. While the UID appears in at least one request of a live process, `libsrpc_proc_destroy()` does not free that UID's user blocks immediately. The hold is bound to a thread: the thread that took it releases it, and it disappears together with the thread or the process.
* **Readiness protocol.** The executor CAS-es the `LIBSRPC_RC_FLAG_LOCK` flag (`0xC0000000`) into `rc`, copies the result, and publishes `LIBSRPC_RC_FLAG_READY` (`0x80000000`). A negative `rc` is the slot's error code, `0` means no reply yet. The wrapper accepts a reply only on an exact match with the ready flag. If the slot is still in the `LOCK` state, it waits up to 100 `pause` iterations.
* **Validation.** `libsrpc_req_is_corrupted()` checks the signature with a double `acquire` read, before and after reading the fields, and the consistency of the sizes. `libsrpc_req_destroy()` clears the signature via CAS, so destroying twice is safe. This protects against use-after-free when the GC and an executor work at the same time.
* **Dispatch policies.** The registry is walked in order: `main_block` first, then `ext_block`.
  * `RPC_SEND_ALL` — the request goes to every executor (`retnum = num_all`).
  * `RPC_SEND_FIRST` — to the first executor that accepted the request (`retnum = 1`).
  * `RPC_SEND_LAST` — to the last non-empty slot of the walk.
  * `RPC_SEND_RR` — to one executor in round-robin (atomic counter `req_send_rr` modulo `num_all`).
  * `LAST` and `RR` have no fallback executor: if the chosen process did not accept the request, the call ends with `ENOTAVAILABLE`.
* **Partial dispatch.** If fewer requests were sent than slots were reserved, the expected number of replies is reduced. If none could be sent, the call ends with `ENOTAVAILABLE`.
* **Timeout.** The caller waits in a `libsrpc_sem_wait_timeout_us()` loop and recounts the ready slots after every wake-up. The interval is set in microseconds:
  * `libsrpc_timeout_oneshot_set()` — the thread's next call;
  * `libsrpc_timeout_func_set()` — a function, within the thread;
  * `libsrpc_timeout_global_set()` — the whole process.

  The default is `LIBSRPC_TIMEOUT_DEFAULT` (1 s), the lower bound is `LIBSRPC_TIMEOUT_MINIMUM` (100 µs). The interval is counted again after every wake-up. If no new reply arrives within the interval, the empty slots are marked `-ERPCWAITIMEDOUT` and the wait ends.

### 10. Recursion and Errors

* **Recursion ban.** Executor threads have the TLS flag `srpc_disable_rpc_recursion` set, so an executor cannot make an RPC call from a handler: the request is not sent, and the thread's code is `ERECURSIVE`.
* **Check in the dispatcher.** Before the call the addresses `&name` and `&sRPCFN(name)` are compared. If they match, the process ended up in the registry without a local implementation and the stub would have called itself. Such a call is not executed, and the slot stays unanswered until the timeout.
* **Own codes.** `libsrpc` extends the system `errno` with codes after `MAX_ERRNO` (4095):

| Code | Value | Meaning |
|---|---|---|
| `ELIBNOINIT` | 4096 | Library not initialized |
| `ESHMNOINIT` | 4097 | Pool not connected |
| `ERPCDISABLE` | 4098 | RPC disabled: no connection to the daemon |
| `ENOREGFUN` | 4099 | The function has no executors |
| `ERECURSIVE` | 4100 | RPC from an executor thread is forbidden |
| `ECANCELLED` | 4101 | The executor disconnected without taking the request |
| `ENOTAVAILABLE` | 4102 | No available executor or slot |
| `ERPCWAITIMEDOUT` | 4103 | No reply within the timeout |
| `ENOTFOUND` | 4104 | The block does not belong to the executor (hold) |
| `EPROCDESTROYED` | 4105 | The executor has already disconnected (hold) |
| `EINVALREQUEST` | 4106 | No usable last request (hold) |
| `EINVALRESPONSE` | 4107 | No usable response slot (hold) |

* **Where the codes live.** The thread's error code is stored in a TLS variable. The wrapper clears it at the start of a call and records the reason if the request could not be sent. It is read through `libsrpc_errno_get()` and decoded by `libsrpc_strerror()`. The fate of a sent request is in its slot's `rc`, and `libsrpc_lastreq_get()` returns it. Timeout and cancellation do not enter the thread's code.

## 🔄 Lifecycle of an RPC Call

1. **Initialization.**
   * The application loads `libsrpc.so` and the constructor with priority 101 runs.
   * The constructor obtains the originals of `malloc`/`calloc`/`realloc`/`free` via `dlsym(RTLD_NEXT)`. By `program_invocation_name` it checks whether the process is the daemon itself. If not, it starts the daemon (a daemon that loses the `bind()` race exits silently) and starts the `unix_client` control thread.
   * The thread connects to the daemon's socket, sends the bitmap of its functions, receives the pool FD, and maps it at the fixed address.
   * The thread finds its `libsrpc_proc_t` and `proc_uid`, starts executor threads for the available CPUs, sets the status to `RUN`, and enables RPC. The constructor waits the whole time and returns control when the process is ready.

2. **The call (caller side).**
   * The application calls `log_write("Hello")`. The symbol resolves to the generated stub `librpcimp_log_write`.
   * The stub reads `num_all` from the registry. If there are no executors, the call ends immediately with `ENOREGFUN`.
   * A cached `libsrpc_request_t` is taken, or a new one is allocated (`libsrpc_shmem_malloc_type(..., LIBSRPC_SHMDT_REQUEST)`).
   * The arguments are copied into `req->buf` and the response slots are laid out.
   * For every chosen executor `resp->hp_proc` is filled in and the request is placed in its MPMC queue. If the executor has sleeping threads, `sem_post` is done on the process semaphore.
   * The calling thread sleeps on `req->sem_wakeup`.

3. **Processing (executor thread).**
   * An executor thread, pinned to its core, takes the request from the queue: either during the short busy wait, or after waking on the process semaphore.
   * It publishes the Hazard Pointer `thread->hp_req` and rechecks the request signature.
   * It finds its slot by `hp_proc`, unpacks the arguments, and calls the real `log_write()`.
   * It writes the result into the slot: `LOCK` → data → `READY`.

4. **Completion.**
   * The executor does `sem_post` on `req->sem_wakeup` and clears the Hazard Pointer.
   * The caller wakes and recounts the ready slots. When all are collected, the wrapper returns the result of slot 0. The rest are available through `libsrpc_lastreq_num()` / `libsrpc_lastreq_get()`.
   * The request block stays in the thread's cache for the next call.

## 💥 Failures and Recovery

* **A process died in the middle of an allocator operation.** The next acquisition of the pool mutex receives `EOWNERDEAD` and rolls the unfinished operation back through the BUL. Application data inside blocks is not rolled back: structures an application keeps in the pool it protects itself, for example with a robust mutex and `EOWNERDEAD` handling.
* **An executor died.** The daemon sees the socket break and calls `libsrpc_proc_destroy()`:
  * the process is removed from the registry and its descriptor is invalidated;
  * requests the executor did not manage to take from the queue receive `-ECANCELLED`, and the callers are woken;
  * a request the executor was already processing stays unanswered, and the caller gets `-ERPCWAITIMEDOUT` on timeout;
  * the process's blocks are walked with `tlsf_free_uid_blocks(pool, proc_uid, ...)`. User blocks are freed immediately if nobody holds their UID, otherwise they are deferred into the GC list. Request blocks are invalidated and marked `dc`, and the process descriptor is released via `libsrpc_shmem_free_dc()`.
* **A caller died.** Its request blocks are invalidated and go to the GC. If the signature was cleared before the executor took the request, the function is not called. If during processing, the function runs but the result is not written. The GC frees the block once the executor's Hazard Pointer is cleared.
* **A hold's holder died.** The hold check looks only at requests of live processes, and a thread's request block is freed when the thread exits. The hold therefore disappears together with its holder, and the deferred blocks return to the pool on the next GC pass. A forgotten `unlock` in a crashed process does not turn into a permanent leak.
* **An executor hung.** The caller gets `-ERPCWAITIMEDOUT` on timeout. On the next call the block with the expired slot is replaced by a new one, and the old one goes to the GC.
* **The daemon died.** Clients see the socket break: RPC in them is disabled (`rpc_enable = false`) and the executor threads stop. There is no reconnect. The old pool's mapping remains in the processes, but new processes start a new daemon with a new pool and never meet the ones already running. The daemon is a single point of failure.

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

* **Anonymous memory (memfd).** The pool is created with `memfd_create`, not `shm_open`. It is not visible in `/dev/shm`, and after the last reference to the descriptor nothing remains: no manual cleanup after crashes is needed.
* **Fixed base address.** The pool contains **absolute** pointers, so every process maps it at `SHMEM_BASE_VADR` (`0x200000000000` by default) with `MAP_FIXED_NOREPLACE`. If the address is already taken, the mapping is honestly not created instead of overwriting someone else's region.
* **Fork protection.** `madvise(..., MADV_DONTFORK)` is called for the pool: a child process does not inherit the pool and cannot accidentally corrupt the allocator's metadata. The child cannot use RPC or the pool after `fork()` either.
* **Multiple cores.** Executor threads are created for the permitted CPUs and pinned to cores through `pthread_attr_setaffinity_np`, which reduces context switches and cache migration.
* **Build isolation through BUILD_TS.** The configuration timestamp is compiled only into `libsrpc.so` and enters the daemon name, the socket name, the pool signature, and the registration-message signature. In effect this is an automatic ABI version: processes that loaded different builds of the library cannot see each other. Applications do not contain the stamp. They need rebuilding when `RPC_LIST` changes, because the prototypes and `libsrpc_funid_<name>` come from it.
* **Build parameters.** Pool size (`SHMEM_SIZE_KB`), base address (`SHMEM_BASE_VADR`), queue capacity (`MPMCQ_DEGREE`), message level (`DBG_LVL`), and disabling allocator substitution (`DISABLE_ALLOC`) are CMake variables and enter the code as `-D` definitions.
* **Strict flags.** The allocator is built with `-Wall -Wextra -Werror -Wformat`, the library with `-Wall -Wextra -Werror -Wconversion -Wshadow`. Unused parameters are marked `__attribute__((unused))`.
* **Tied to x86_64.** The busy wait uses `__builtin_ia32_pause`, and the library is built with `-mrdrnd`. On other architectures the code will not build without changes.

## ⚠️ Known Limitations

* **The timeout is counted from the last reply.** When dispatching to several executors, every reply that arrives restarts the interval. In the worst case the total wait approaches the number of executors times the timeout.
* **An unsent call can return an old value.** If the request could not be sent (`ERPCDISABLE`, `ERECURSIVE`, `ENOTAVAILABLE`), the wrapper reads slot 0 of the reused block. It may still hold the result of this thread's previous call, and `libsrpc_lastreq_get(0)` returns `0` for it too. For functions that return a value, the reason code is sometimes replaced by `ETIMEDOUT`. The reliable sign of success is `libsrpc_errno_get() == 0` **and** `0` in the wanted slot.
* **Error codes of the memory functions.** `TLSF_ERR_*` codes, whose numbers coincide with the system `errno`, end up in the thread's code. `libsrpc_strerror()` decodes them wrongly; `tlsf_strerror()` is needed.
* **Client credentials are not checked.** Only the PID is taken from `SO_PEERCRED`. Any process that connects to the abstract socket with the correct signature receives an FD of the whole pool for reading and writing.
* **The daemon is a single point of failure.** There is no reconnect to a new daemon (see Failures and Recovery).
* **`libsrpc_fnreg_num_get()` without a pool.** The function does not check whether the pool is connected. If the connection to the daemon failed, the call touches memory that does not exist.
* **The cost of the busy wait.** Every process keeps executor threads, one per CPU, and after every request they spin for a while before sleeping.
* **A fixed pool.** The pool size is set at build time and does not grow. The number of simultaneously connected processes is limited by the 16-bit `proc_uid`.

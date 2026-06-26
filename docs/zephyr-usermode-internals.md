# Zephyr User-Mode Multi-Thread: Platform Internals

Reference for understanding why `WAMR_BUILD_ZEPHYR_USERMODE_MT` exists, what it changes, and which Zephyr Kconfigs interact with it.

## The Kernel-Object Registration Gap

Zephyr's `CONFIG_USERSPACE` adds syscall validation to every kernel API call. Every kernel object (mutex, semaphore, condvar, thread, message queue) must be registered in a kernel object table for the syscall handler to verify that the calling thread has permission to use it.

Registration happens two ways:

- **Static definitions** (`K_MUTEX_DEFINE`, `K_SEM_DEFINE`) are scanned at build time by a gperf-based tool that generates a perfect-hash table.
- **Dynamic allocation** (`k_object_alloc`) registers at runtime into the same table.

WAMR allocates all its internal kernel objects — the heap lock mutex, bh_queue's lock and condvar, thread join semaphores — **inside its own memory pool** via `BH_MALLOC`. These objects are invisible to Zephyr:

- They are not statically defined, so the gperf scanner never sees them.
- They live inside `wamr_partition` app memory. Even if they were in `.bss`, the scanner doesn't scan partition sections.
- Every syscall on these objects fails with `-EINVAL` or a protection fault.

`zephyr_library_app_memory(wamr_partition)` in the platform CMake rewrites the linker rules so that **all globals** in `wamr_lib.a` go into the partition's `.data` / `.bss` sections. The gperf scanner scans only the kernel's standard data sections, not partition sections. Anything statically defined in a partitioned translation unit is invisible.

| Global in wamr_lib.c (partitioned) | Registered? |
|---|---|
| `static struct k_thread iwasm_user_mode_thread;` | NO |
| `K_THREAD_STACK_DEFINE(stack, ...)` | NO (stack itself + its kobject metadata) |
| `K_APPMEM_PARTITION_DEFINE(wamr_partition)` | n/a — needs to be visible to `gen_app_partitions.py` from a non-partitioned TU; placing it inside the partition it describes is also a chicken-and-egg |
| `struct k_mem_domain wamr_domain;` | n/a — `k_mem_domain` is not a kobject type at all |

Additionally:
- WAMR's condvar was hand-rolled using semaphores (not Zephyr's native `k_condvar`), so it couldn't work through syscalls.
- `os_thread_create` always created kernel-mode threads with no permission inheritance.
- `os_thread_join` had a race where fast-exiting threads couldn't be joined.

## Fix 1: Dynamic Kobject Allocation

Under `WAMR_BUILD_ZEPHYR_USERMODE_MT=1`, all kernel object types (`zmutex_t`, `zsem_t`, `zcond_t`) become **pointers** to objects allocated via `k_object_alloc()`. This function both allocates memory from Zephyr's system heap AND registers the object in the kernel object table in one call.

```c
/* Before: value type in WAMR heap — NOT registered, fails from user mode */
#define zmutex_t struct k_mutex

/* After: pointer to k_object_alloc'd object — registered, works */
#define zmutex_t struct k_mutex *
#define zmutex_init(mtx) do { \
    *(mtx) = k_object_alloc(K_OBJ_MUTEX); \
    if (*(mtx)) k_mutex_init(*(mtx)); \
} while (0)
```

The macros dereference transparently — no call-site changes are needed anywhere in WAMR. `zmutex_lock(&lock, timeout)` expands to `k_mutex_lock(*(&lock), timeout)` which is `k_mutex_lock(lock, timeout)` where `lock` is the registered pointer.

Destruction uses `k_object_release()` (a proper syscall, safe from user mode) instead of `k_object_free()` (not a syscall, causes GPF from user mode).

The condvar implementation was replaced entirely: the hand-rolled semaphore-based wait list became Zephyr's native `k_condvar` (available since Zephyr 2.7+), also allocated via `k_object_alloc(K_OBJ_CONDVAR)`.

Zephyr provides runtime equivalents that bypass the gperf scanner:

| Static (broken inside partition) | Dynamic (works) | Requires |
|---|---|---|
| `struct k_thread foo` | `k_object_alloc(K_OBJ_THREAD)` | `CONFIG_DYNAMIC_OBJECTS=y` |
| `K_THREAD_STACK_DEFINE(s, sz)` | `k_thread_stack_alloc(sz, K_USER)` | `CONFIG_DYNAMIC_THREAD=y` |
| `struct k_mutex / k_sem / k_condvar` | `k_object_alloc(K_OBJ_MUTEX/SEM/CONDVAR)` | `CONFIG_DYNAMIC_OBJECTS=y` |
| `K_APPMEM_PARTITION_DEFINE` | *no dynamic equivalent* | — |
| `struct k_mem_domain dom` | *not a kobject — keep as plain static* | — |

`k_mem_domain` is operated on by the supervisor side of `k_mem_domain_init` / `k_mem_domain_add_thread` and is reachable from a user thread only via the thread struct's `mem_domain_info` pointer. The domain struct itself never goes through a syscall validation, so leaving it as a plain static is fine.

## Fix 2: Permission Inheritance via K_INHERIT_PERMS

Once kernel objects are properly registered, the next problem is: how do child threads get access to them? Manually calling `k_object_access_grant()` for every mutex, semaphore, and condvar on every new thread would be impractical.

`os_thread_create_with_prio()` now detects user context via `k_is_user_context()` and automatically sets `K_USER | K_INHERIT_PERMS` on the new thread. `K_INHERIT_PERMS` is the key: the child thread automatically inherits access to **all** kernel objects the parent has — bh_queue's mutex/condvar, the WAMR heap lock, everything. No manual grants needed.

| | Kernel mode (original) | User mode (new) |
|---|---|---|
| Thread object | `BH_MALLOC(sizeof(thread_obj_node))` with embedded `struct k_thread` | `k_object_alloc(K_OBJ_THREAD)`, pointer stored in `thread_obj_node.dyn_tid` |
| Thread flags | `0` | `K_USER \| K_INHERIT_PERMS` |
| Object tracking | Single unified `thread_obj_node` list with `is_dyn` discriminator (both modes share one list and one reclaim function) | Same list, `is_dyn = true` selects the release path |
| Cleanup | `BH_FREE(node)` | `k_object_release(dyn_tid); BH_FREE(node)` |

### MPU Stack Chicken-and-Egg

One chicken-and-egg problem remains: `os_thread_create()` uses static MPU-aligned stacks (`K_THREAD_STACK_ARRAY_DEFINE`). When a user-mode thread calls `k_thread_create()`, Zephyr validates that the **calling** thread has permission to the stack being passed. But the user-mode thread can't grant itself access to stacks it doesn't own yet.

Solution: `os_thread_env_init_for_usermode(tid)` is called from kernel `main()` before starting the user-mode thread, granting it access to all WAMR thread stacks upfront.

## Fix 3: Linker --undefined Hints for k_condvar

Zephyr links libraries in two sections:

```
--whole-archive:    app, libzephyr.a, drivers, libc   (all symbols kept)
--no-whole-archive: libkernel.a, then wamr_lib.a      (on-demand extraction)
```

`k_mutex` and `k_sem` survive because Zephyr's own `--whole-archive` code references them (picolibc's `locks.c` uses mutex, `mpsc_pbuf.c` uses semaphore). The linker extracts those `.o` files from libkernel.a during its single pass.

`k_condvar` has no such reference — nothing in Zephyr's core uses condvar. By the time the linker reaches wamr_lib.a and sees the condvar references, the condvar `.o` in libkernel.a has already been skipped.

The platform's CMakeLists.txt (in `product-mini/platforms/zephyr/`) pre-marks condvar symbols as needed before file scanning begins:

```cmake
zephyr_link_libraries(
  -Wl,--undefined=z_impl_k_condvar_init
  -Wl,--undefined=z_impl_k_condvar_signal
  -Wl,--undefined=z_impl_k_condvar_wait
  -Wl,--undefined=z_impl_k_condvar_broadcast
)
```

This ensures the linker extracts the condvar `.o` from libkernel.a before it scans wamr_lib.a.

## Fix 4: Thread Join Race

The original `os_thread_join()` had a race: if the target thread exited before `os_thread_join()` was called, `os_thread_cleanup()` would remove and free the `thread_data`, making it impossible for the joiner to find it. The join would print "Can't join thread, probably already exited" and return without actually synchronizing.

The fix separates "thread finished" from "someone joined it":

- `os_thread_cleanup()` sets `thread_exited = true` and signals waiters, but **does not** remove or free `thread_data`.
- `os_thread_join()` finds `thread_data` via lookup (it's still in the list), checks the flag:
  - Thread still running → blocks on semaphore (existing path), then cleans up.
  - Thread already exited → cleans up immediately, no wait needed.
- `thread_data_destroy()` handles the actual removal and freeing, called by the joiner after synchronization is complete.

This fix is **unconditional** in WAMR — it applies to both kernel-mode and user-mode multi-threading. It was discovered while testing user-mode multi-thread but benefits all threading scenarios.

## When the Flag is Needed

The flag `WAMR_BUILD_ZEPHYR_USERMODE_MT=1` is needed when **all** of the following are true:

- `CONFIG_USERSPACE=y` (Zephyr enforces syscall-based kobject validation)
- WAMR's threading API in use (`os_thread_create`, `bh_queue`, etc.)
- One of:
  - Multiple user-mode threads, OR
  - A user-mode thread that uses bh_queue's condvar (any contention path that takes a slow syscall)

The runtime fault when the flag is missing occurs at `k_mutex_init` inside `bh_queue_create` (per Task 8's empirical finding), not at the first condvar wait. The failure mode is:

```
<err> os: 0x... is not a valid k_mutex
<err> os: address is not a known kernel object
<err> os: Page fault at address 0x... (error code 0x4)
```

| Scenario | Flag needed? |
|---|---|
| Kernel-mode only (any threading model) | NO |
| User-mode single-thread (no bh_queue contention) | NO |
| User-mode single-thread using bh_queue condvar | YES |
| User-mode multi-thread | YES |

Zephyr-side prerequisites (required when the flag is set):

- `CONFIG_USERSPACE=y`
- `CONFIG_DYNAMIC_OBJECTS=y`
- `CONFIG_HEAP_MEM_POOL_SIZE` > 0 (k_object_alloc draws from this heap)

## Zephyr-App vs. Zephyr-Library Shape

WAMR on Zephyr can be built in two shapes:

- **Zephyr-library shape** (recommended): `wamr_lib.a` linked via `--no-whole-archive`. Application code is a thin entry point. The library is self-contained.
- **Zephyr-app shape**: WAMR code compiled directly into the application's `--whole-archive` side.

### What Cannot Move from Application to Library

| Element | Where it must live | Reason |
|---|---|---|
| `K_APPMEM_PARTITION_DEFINE(wamr_partition)` | non-partitioned TU (e.g. `main.c`) | The partition's metadata struct cannot live inside the partition it describes; `gen_app_partitions.py` looks for the `K_APP_DMEM_SECTION` symbols from outside as well |
| `iwasm_user_mode()` (the supervisor-side launcher itself) | callable from `main()` | `k_thread_create` with `K_USER`, `k_mem_domain_add_thread`, and `k_wakeup` must run from supervisor mode; that path itself can be in `wamr_lib.c` as long as `main()` invokes it before any user-mode thread takes over |

The minimum that must remain in `main.c` is a single `K_APPMEM_PARTITION_DEFINE` plus a one-line `main()` calling `iwasm_user_mode()`. Everything else — thread creation, stack allocation, domain init, the user-mode entry — moves cleanly into the library.

### Trade-offs

| | Zephyr-app (everything in main.c) | Zephyr-library (mostly in wamr_lib.c) |
|---|---|---|
| Library is self-contained | no — application must replicate setup | yes — application is a 3-line stub |
| Compile-time kobject registration | yes (gperf) | no (runtime only) |
| Requires `CONFIG_DYNAMIC_OBJECTS` / `DYNAMIC_THREAD` | no | yes |
| Requires non-zero `HEAP_MEM_POOL_SIZE` | no | yes |
| Linker `--undefined` hints needed | no | yes (`k_condvar_*` symbols) |
| Failure mode if a config is missing | n/a | `iwasm_user_mode()` returns false with a clear `printk` |

The dynamic path is the same path the multi-threaded user-mode work already takes for WAMR-internal kernel objects, so it doesn't add a new dependency category — just extends the existing one to the entry-point thread itself.

### Why a "Pure Zephyr-App" User-Mode Sample Was Not Added

During this work an attempt was made to add a separate sample (`product-mini/platforms/zephyr/user-mode-app/`) shaped like `simple/`: `target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE} src/main.c)`, with no `zephyr_library` and no `zephyr_library_app_memory(wamr_partition)`. The goal was to exercise the same flag-toggle demo (`WAMR_BUILD_ZEPHYR_USERMODE_MT=1` vs. `=0`) but in the simplest possible Zephyr-app layout.

It does not work, and the obstacle is structural rather than fixable in a sample alone:

- WAMR has many static globals (the heap pool, mutex pool, thread tracking lists, the `mpu_stacks` array, internal lookup tables, etc.). Under `CONFIG_USERSPACE`, any global that a user-mode thread reads or writes must live inside a memory partition the thread has access to. The Zephyr-native way to put a global into a partition is the `K_APP_DMEM(partition)` / `K_APP_BMEM(partition)` macros placed on the definition itself.
- When WAMR is compiled directly into `app` via `target_sources(app ...)`, its `.data` and `.bss` sections land in the kernel's default sections, not in `wamr_partition`. Every WAMR-internal access from a user-mode thread then triggers an MPU fault.
- Fixing this in the sample would require either (a) adding `K_APP_DMEM(wamr_partition)` to every WAMR-internal global definition — a core-WAMR change that couples the platform-agnostic runtime to Zephyr macros, or (b) post-hoc relocating WAMR's sections at link time, which is what `zephyr_library_app_memory(wamr_partition)` already does for a `zephyr_library`. Option (b) is the only sane choice — and once you take it, you are no longer in pure Zephyr-app shape; the WAMR runtime *is* a Zephyr library again, with only the application's `main.c` left on the `app` target.

The end state — `wamr_lib` as a `zephyr_library` with `zephyr_library_app_memory(wamr_partition)`, application code on the `app` target — is what `product-mini/platforms/zephyr/user-mode/` already provides. A second sample with the same shape adds maintenance without adding coverage, so the sample was dropped. The flag-toggle demo (turning `WAMR_BUILD_ZEPHYR_USERMODE_MT` on and off) is exercised by `user-mode/` instead, which already builds in three of the four (flag × ST/MT) cells and rejects the fourth at CMake time.

The takeaway for users targeting Zephyr: if WAMR sources are anywhere on the user-mode access path, they must be in a TU that goes through `zephyr_library_app_memory(...)`. Pulling WAMR into the `app` target directly works only for kernel-mode builds.

### Why Not Split: Kernel-Mode WAMR Engine + User-Mode WASM Execution Thread

A natural-looking third shape is to put the WAMR runtime (interpreter/AOT engine, heap, module loader) in kernel mode and run only the WASM execution itself in a user-mode thread, hoping to combine "no kobject-registration gymnastics for the engine" with "MPU isolation for the WASM sandbox". This does not work on Zephyr and is worth recording so the design isn't re-attempted:

- **The interpreter and the WASM linear memory are not separable.** Every WASM opcode read, local-variable access, host-function dispatch, and heap allocation touches data structures that are intertwined with the WASM module instance. Splitting "engine code" from "module data" across modes would require a syscall on every opcode, which is performance-fatal even for trivial WASM programs.
- **Zephyr has no cheap mode transition.** The only kernel→user transition is `k_thread_user_mode_enter()`, which is **terminal** — the calling thread becomes user mode permanently. The reverse direction is the syscall path, which targets kernel APIs registered at build time, not arbitrary WAMR functions. There is no `enter_user_mode_for_one_function()` primitive.
- **Kernel-mode threads bypass the MPU.** A `wamr_partition` only restricts user-mode threads. If WAMR runs kernel-mode and the WASM linear memory lives in `wamr_partition`, the kernel-mode engine reads and writes the WASM heap unrestricted — which defeats the isolation goal the split was supposed to deliver.
- **Cross-mode host-function calls would need custom syscalls.** Native imports, WAMR exception throw, GC barriers, and module lookup all live engine-side. A user-mode WASM thread calling back into the engine would need a hand-written `z_vrfy_*` validator per call site. Zephyr syscalls are not generic — they target specific kernel APIs.

The Zephyr model offers two clean choices and no useful middle ground: **all kernel mode** (cheap, isolation rests on WAMR's bounds checking) or **all user mode** (more setup as documented above, isolation rests on both WAMR's bounds checking and the MPU partition). The hybrid path delivers strictly less isolation than the all-user shape at strictly higher complexity than the all-kernel shape. If you want stronger isolation than what an MPU partition can provide, you need a different OS (one with per-process MMU contexts), not a different WAMR layout.

One narrower variant *can* be made to work but isn't documented as a sample yet: keep WAMR in kernel mode but place **only the WASM linear memory** (not the engine's data structures) into a `wamr_partition` and grant a user-mode "viewer" thread read access. The kernel-mode interpreter reads/writes the partition via plain pointers (kernel reads user memory freely), and a separate user-mode thread can inspect WASM memory without escalating. This protects host-side data from a hypothetically-compromised WASM module while keeping the engine fast — but requires modifying WAMR's heap allocator to place WASM-instance memory specifically in the partition, which is a core-WAMR change beyond what this branch is for.

## Configuration Cheat Sheet

### prj.conf for User-Mode Multi-Thread

```conf
# Zephyr prerequisites
CONFIG_USERSPACE=y
CONFIG_DYNAMIC_OBJECTS=y
CONFIG_HEAP_MEM_POOL_SIZE=4096   # k_object_alloc draws from this heap

# Optional: only if you also use k_thread_stack_alloc for the entry thread
CONFIG_DYNAMIC_THREAD=y
CONFIG_DYNAMIC_THREAD_POOL_SIZE=2
CONFIG_DYNAMIC_THREAD_STACK_SIZE=2048
```

Critical: `k_object_alloc` / `k_thread_stack_alloc` draw from the system heap (sized by `HEAP_MEM_POOL_SIZE`). Default is 0; without bumping it, every dynamic allocation returns NULL and the user-mode thread never starts. The first failure mode is "Failed to allocate thread object." printed from `iwasm_user_mode()`.

### CMake — Sample-Side

```cmake
# Enable the flag
set(WAMR_BUILD_ZEPHYR_USERMODE_MT 1)
```

### CMake — Platform-Side (Already in product-mini/platforms/zephyr/)

Pre-mark the condvar symbols as needed before file scanning:

```cmake
if(WAMR_BUILD_ZEPHYR_USERMODE_MT)
  zephyr_link_libraries(
    -Wl,--undefined=z_impl_k_condvar_init
    -Wl,--undefined=z_impl_k_condvar_signal
    -Wl,--undefined=z_impl_k_condvar_wait
    -Wl,--undefined=z_impl_k_condvar_broadcast
  )
endif()
```

## Result

The user-mode sample (`product-mini/platforms/zephyr/user-mode/`) demonstrates the full architecture:

```
kernel main()                          (supervisor mode)
  ├── set up memory domain (wamr_partition + z_libc_partition)
  ├── os_thread_env_init_for_usermode()  grant MPU stack access
  └── spawn wamr_main thread           (K_USER, K_FOREVER → start after setup)

iwasm_main()                           (user mode, in wamr_lib.c)
  ├── wasm_runtime_full_init()          pool allocator, heap lock via k_object_alloc
  ├── wasm_runtime_load()               load WASM module
  ├── bh_queue_create()                 queue lock + condvar via k_object_alloc
  └── os_thread_create() × N workers   (K_USER | K_INHERIT_PERMS, automatic)
        ├── wasm_runtime_instantiate()  each worker gets its own WASM instance
        ├── wasm_application_execute_main()  run the WASM app
        ├── bh_post_msg() × M          send messages to shared bh_queue
        └── wasm_runtime_deinstantiate()

iwasm_main() consumer loop             (user mode)
  ├── bh_get_msg(BHT_WAIT_FOREVER)     blocks on k_condvar
  └── os_thread_join() × N             wait for workers to finish
```

Tested on both qemu_x86 (x86-32) and qemu_arc (ARC HS). All WAMR code runs in user mode with proper kernel object registration, permission inheritance, and clean thread lifecycle management.

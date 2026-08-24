# WAMR guest pthreads in Zephyr user mode

This sample runs WAMR itself inside a Zephyr user-mode root thread and lets the
guest create two pthread workers from a caller-supplied static Zephyr pool. It
documents the final integration that landed across Tasks 1 to 8:

- thread identities are opaque `korp_tid` handles, not raw `k_tid_t`
- user-mode mutexes and conditions are opaque `korp_mutex` / `korp_cond`
  handles backed by native `struct k_mutex` and `struct k_condvar`
- the supervisor prepares the thread pool and synchronization pool separately
  for the suspended WAMR root thread before that thread starts
- the guest uses only mutex and condition APIs, which are the Zephyr pthread
  subset currently supported in this path

## Supervisor-owned storage

The supervisor owns every Zephyr kernel object statically:

```c
WAMR_ZEPHYR_THREAD_POOL_DEFINE(wamr_threads, 4, 4096);
WAMR_ZEPHYR_SYNC_POOL_DEFINE(wamr_sync, 16, 8);
K_APPMEM_PARTITION_DEFINE(wamr_partition);
K_THREAD_STACK_DEFINE(wamr_root_stack, 8192);
```

That splits storage into three different classes:

- `struct k_thread` objects for guest pthread workers
- `K_THREAD_STACK_*` storage for their user stacks
- native mutex / condition arrays for guest synchronization

`struct k_thread` and `K_THREAD_STACK_DEFINE()` /
`K_THREAD_STACK_ARRAY_DEFINE()` storage are kernel objects, not ordinary
application globals, so they are not routed by
`zephyr_library_app_memory(wamr_partition)`. The app-memory partition is still
required for WAMR's mutable globals and global heap pool, but Zephyr thread
objects, stacks, mutexes, and condition variables must be prepared and granted
as kernel objects.

## Preparation sequence

The root thread is created suspended and prepared in supervisor mode:

```c
root = k_thread_create(&wamr_root_thread, wamr_root_stack,
                       K_THREAD_STACK_SIZEOF(wamr_root_stack),
                       wamr_runtime_main, NULL, NULL, NULL,
                       WAMR_ROOT_PRIORITY, K_USER, K_FOREVER);

k_mem_domain_init(&wamr_domain, ARRAY_SIZE(parts), parts);
k_mem_domain_add_thread(&wamr_domain, root);
wamr_zephyr_thread_pool_prepare(&wamr_threads, root);
wamr_zephyr_sync_pool_prepare(&wamr_sync, root);
k_thread_start(root);
```

The legacy `user-mode` sample now follows the same rule for synchronization:
create the WAMR root suspended, add `wamr_partition` plus
`z_libc_partition`, prepare the static sync pool for that exact root, then
start it.

The two preparation APIs solve different problems and are intentionally
separate:

- `wamr_zephyr_thread_pool_prepare()` grants the WAMR root access to the
  static `struct k_thread` objects and stack objects that future guest pthreads
  will consume.
- `wamr_zephyr_sync_pool_prepare()` initializes the management mutex, native
  mutex array, and native condition array, records them in WAMR metadata, and
  grants them to the same root thread.

Both calls are supervisor-only, idempotent for the same owner and descriptor,
and reject replacement with a different live pool.

## Opaque WAMR handles and permissions

In user mode the WAMR platform types are deliberately opaque:

```c
struct korp_thread_handle;
typedef struct korp_thread_handle *korp_tid;
struct korp_mutex_handle;
struct korp_cond_handle;
typedef struct korp_mutex_handle *korp_mutex;
typedef struct korp_cond_handle *korp_cond;
```

Guest-visible pthread values and public WAMR handle variables store only those
opaque handles. They do not expose raw `k_tid_t`, `struct k_mutex *`, or
`struct k_condvar *`. Private platform metadata deliberately retains the native
identities and pointers needed to resolve each handle before calling
`k_thread_*`, `k_mutex_*`, or `k_condvar_*`; the real Zephyr objects were
already granted during pool preparation.

This is also why `zephyr_library_app_memory(wamr_partition)` and
`k_object_access_grant()` are different tools:

- the memory domain makes WAMR globals, the global heap pool, and libc state
  writable from the user thread
- kernel-object permissions make specific Zephyr thread, stack, mutex, and
  condition objects usable through syscalls

Child user threads inherit the parent's memory domain automatically. Kernel
object permissions are copied separately with `K_INHERIT_PERMS` when WAMR
creates guest pthread workers.

## Fixed pool behavior

The sample uses a four-slot, 4096-byte thread pool because the sample's guest
pthread stack contract is fixed at 4096 bytes:

```cmake
add_compile_definitions(APP_THREAD_STACK_SIZE_DEFAULT=4096)
add_compile_definitions(APP_THREAD_STACK_SIZE_MIN=4096)
```

That gives deterministic failure modes:

- `WAMR_ZEPHYR_THREAD_POOL_DEFINE(..., count, ...)` fails the build if the pool
  is empty or larger than the four bookkeeping slots compiled into the Zephyr
  port
- `wamr_zephyr_thread_pool_prepare()` rejects malformed descriptors such as
  zero stack size or impossible stack stride
- `os_thread_create()` in user mode fails when all four slots are busy
- `os_thread_create()` in user mode also fails if the guest requests a stack
  size larger than the prepared pool stack size

The synchronization pool is equally fixed:

- `WAMR_ZEPHYR_SYNC_POOL_DEFINE(wamr_sync, 16, 8)` provisions 16 mutex slots
  and eight condition slots, and fails the build if either capacity is zero or
  larger than the compiled bookkeeping limits
- `wamr_zephyr_sync_pool_prepare()` rejects malformed, aliased, overlapping, or
  oversized descriptors
- the runtime does not allocate dynamic Zephyr mutexes or condition variables

No dynamic Zephyr objects are required anywhere in this sample.

## Synchronization handle lifecycle

Mutexes and conditions have their own fixed-slot lifecycle, separate from the
thread join/detach rules below.

`os_mutex_init()` and `os_cond_init()` scan only their own prepared native
arrays for a `FREE` slot, reserve it, publish a fresh opaque handle, and mark
the slot `ACTIVE`. The two pools are independent:

- exhausting mutex slots returns `BHT_ERROR` from `os_mutex_init()` without
  consuming any condition capacity
- exhausting condition slots returns `BHT_ERROR` from `os_cond_init()` without
  consuming any mutex capacity

Destroy is allowed only when the slot is quiescent:

- `os_mutex_destroy()` requires no in-flight operation, no owner, and no
  recursion depth
- `os_cond_destroy()` requires no in-flight operation and no active waiter

On success destroy clears the caller's handle, invalidates the slot metadata,
and returns that native-backed slot to `FREE` for a later reuse cycle. A locked
or referenced mutex, or a condition with an active waiter, is rejected with
`BHT_ERROR` instead of tearing down the native Zephyr object underneath live
users.

Reused slots always receive a new opaque generation handle. Stale, foreign,
and double-destroy handles therefore fail WAMR's metadata lookup with
`BHT_ERROR` before any `k_mutex_*` or `k_condvar_*` call reaches Zephyr's
syscall verifier.

## Supported pthread subset

This path is intentionally narrow. The guest fixture uses:

- `pthread_create`
- `pthread_join`
- `pthread_mutex_*`
- `pthread_cond_wait`
- `pthread_cond_signal`
- `pthread_cond_broadcast`

Named semaphore APIs are still unsupported here:

```c
os_sem_open(...)   -> NULL
os_sem_close(...)  -> BHT_ERROR
os_sem_unlink(...) -> BHT_ERROR
```

The task also did not expand rwlock scope. `korp_rwlock` remains outside this
sample's contract, and the sample README does not claim guest rwlock support.

Zephyr's `k_work_user_queue_start()` is useful reference evidence for the same
ownership split: supervisor code owns thread and stack storage, then grants a
user worker a prepared execution context. It is not a replacement for
`os_thread_create()`, because a work queue does not provide guest-controlled
pthread creation, join/detach lifecycles, or WAMR's opaque handle mapping.

## Joinable and detached lifecycles

Joinable guest threads keep their slot until WAMR joins them:

```text
create -> running -> exited -> join -> slot released
```

Detached guest threads release the slot only after Zephyr cleanup for that
generation is complete:

```text
create -> running -> detach -> detached-running -> exit -> slot released
create -> running -> exit -> detach -> immediate cleanup -> slot released
```

That matters because the static `struct k_thread` storage can be reused. WAMR's
opaque handle generation prevents a stale detached handle from joining or
claiming a replacement thread that later reuses the same Zephyr slot.

## Guest fixture

The embedded guest source in [src/wasm-app/main.c](./src/wasm-app/main.c)
creates exactly two workers. Both wait on one mutex/condition pair until the
main guest thread broadcasts release, then each adds `21` to the shared
counter. Success is the literal final counter value `42`.

The embedded fixture lives in [src/test_wasm.h](./src/test_wasm.h) so normal
Zephyr and Twister builds do not need wasi-sdk. To regenerate it after editing
the guest source:

```sh
cd product-mini/platforms/zephyr/user-mode-multi-thread/src/wasm-app
./build.sh
```

## Build and test

`sample.yaml` declares one public Twister scenario on `qemu_arc/qemu_arc_hs`.
Run it either through the helper wrapper:

```sh
cd product-mini/platforms/zephyr
python3 build_and_run.py --sim qemu_arc user-mode-multi-thread
```

or directly from a prepared Zephyr workspace:

```sh
west twister -T modules/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode-multi-thread \
  -p qemu_arc/qemu_arc_hs \
  -x EXTRA_ZEPHYR_MODULES=$PWD/modules/wasm-micro-runtime \
  --disable-warnings-as-errors
```

Expected successful console line:

```text
PASS: two guest pthread workers completed in Zephyr user mode
```

Last verified on 2026-08-21:

| Scenario | Simulator | Result |
| --- | --- | --- |
| `sample.wamr.user_mode_multi_thread` | `qemu_arc/qemu_arc_hs` | passed |

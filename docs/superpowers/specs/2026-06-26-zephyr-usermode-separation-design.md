# Zephyr User-Mode Multi-Thread: Platform Separation and Sample Matrix

**Date:** 2026-06-26
**Affected:**
- `core/shared/platform/zephyr/platform_internal.h`
- `core/shared/platform/zephyr/zephyr_thread.c`
- `core/shared/platform/zephyr/zephyr_thread_usermode.c` (new)
- `core/shared/platform/zephyr/shared_platform.cmake`
- `product-mini/platforms/zephyr/simple/` (upgraded to kernel-mode MT)
- `product-mini/platforms/zephyr/user-mode/` (toggle ST/MT, toggle flag)
- `product-mini/platforms/zephyr/user-mode-app/` (new)
- `product-mini/platforms/zephyr/user-mode/USERMODE.md`,
  `USERMODE_MULTITHREAD.md` (relocated/folded into per-sample READMEs)

## Goals

The current branch (`dev/user_mode_multithread_fix`, commit `37b265f0`)
adds Zephyr user-mode multi-thread support to WAMR's `zephyr/` platform
layer. It works, but two structural problems remain:

1. The platform-layer change is auto-gated on
   `CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS`, which means any Zephyr
   build that turns those Kconfigs on for unrelated reasons gets dragged
   onto the dynamic-kobject path (pointer-typed mutex/sem, dual cleanup
   paths, replaced condvar implementation, mandatory linker `--undefined`
   hints). Most such consumers don't need the dynamic-kobject machinery
   — only WAMR multi-thread under user mode does.
2. There is only one sample (`user-mode/`) exercising the new code, and
   it covers exactly one shape (Zephyr-as-library, multi-thread). The
   Zephyr-as-app shape isn't exercised in user mode, and there's nothing
   that *demonstrates* the platform flag is required (the proof is
   prose-only in `USERMODE_MULTITHREAD.md`).

This spec rewires gating onto an explicit WAMR opt-in flag, consolidates
the duplicated code paths in `zephyr_thread.c`, splits the user-mode-only
helpers into a separate translation unit, and reorganizes the sample tree
so each sample has a clear, distinct purpose — including a sample whose
*explicit job* is to demonstrate the flag's necessity by toggling it.

## Non-goals

- Changing the runtime semantics of the multi-thread user-mode path.
  The current sample's worker-pool + bh_queue + condvar flow stays
  exactly as it is in commit `37b265f0` when the flag is on.
- Removing or hiding the join-race fix in `os_thread_cleanup` /
  `os_thread_join`. That's an orthogonal bug fix benefiting all
  consumers and stays unconditional in `zephyr_thread.c`.
- Adding Zephyr-as-app + user-mode + multi-thread coverage. Doable
  later (one CMakeLists swap from the library-shape sample) but
  intentionally out of scope here — would duplicate `user-mode/`'s MT
  mode without adding meaningful coverage.
- Touching any non-Zephyr platform layer.

## Background

### What the branch currently changes

Branch `dev/user_mode_multithread_fix` is one commit on top of `main`,
~709 lines added / 211 removed across:

- `core/shared/platform/zephyr/platform_internal.h` — `zmutex_t`/`zsem_t`
  become pointer types when `CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS`
  is set, allocated via `k_object_alloc()`. `korp_cond` switches to
  native `k_condvar`.
- `core/shared/platform/zephyr/zephyr_thread.c` — `os_thread_create`
  detects `k_is_user_context()` and switches between two parallel code
  paths: kernel-mode (`BH_MALLOC`'d `os_thread_obj`, tracked in
  `thread_obj_list`) and user-mode (`k_object_alloc(K_OBJ_THREAD)`'d
  `korp_tid`, tracked in a new `dyn_thread_list`). Adds
  `os_thread_env_init_for_usermode()` for the MPU-stack chicken-and-egg.
  Fixes a real `os_thread_join` race where fast-exiting threads couldn't
  be joined.
- `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/CMakeLists.txt`
  — `--undefined=` linker hints for `k_condvar_*` symbols (needed
  because nothing in Zephyr's `--whole-archive` side references them
  and they'd otherwise be skipped during the single-pass link).
- `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/wamr_lib.c`
  + `src/main.c` — rewritten from the single-thread "hello world"
  demo into an N-worker + bh_queue + condvar demo.
- `product-mini/platforms/zephyr/user-mode/prj.conf` — adds
  `CONFIG_DYNAMIC_OBJECTS=y`, `CONFIG_HEAP_MEM_POOL_SIZE=4096`.
- Two design notes: `USERMODE.md` and `USERMODE_MULTITHREAD.md`.

### Why a separate WAMR flag is the right gating axis

The natural-looking gate is `CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS`
— if you have those Kconfigs on, you must need the dynamic-kobject code
path, right? No. Zephyr's `sys_mutex`/`sys_sem` have an atomic-CAS fast
path that never enters the kernel under no contention. A single-threaded
user-mode WAMR build (e.g., the pre-branch `user-mode/` sample) takes
that fast path for every mutex op and never touches kobject validation
— so the unregistered heap-allocated locks "work" even though they're
invisible to the kernel.

The dynamic-kobject machinery is only needed when **all three** hold:
- `CONFIG_USERSPACE=y`
- Multiple user-mode threads (or one user-mode thread + condvar use)
- WAMR's threading API in use (which is what generates the
  heap-allocated kobjects that need registration)

The build owner — sample CMakeLists, or downstream — is the only one
who knows whether that combination applies. So the gate should be a
WAMR-side opt-in (`WAMR_BUILD_ZEPHYR_USERMODE_MT=1`), not derived from
Zephyr Kconfigs.

### Why the existing dual code paths can be consolidated

Looking at `zephyr_thread.c` post-branch:
- `os_thread_obj` (kernel-mode) and `dyn_thread_node` (user-mode) are
  both "tid + to-be-freed flag + next pointer" with different cleanup
  destinations (`BH_FREE` vs. `k_object_release`). They can be unified
  to one node type that records *how* it was allocated.
- `thread_obj_list_reclaim` and `dyn_thread_list_reclaim` then merge
  into one function with a per-node branch.
- The user-mode-only helpers (`os_thread_env_init_for_usermode`,
  `dyn_thread_list_add/mark_freed`, anything that touches
  `k_object_alloc` directly) belong in a small companion TU compiled
  only when the flag is on.

## Design

### Sub-project 1: Platform-layer flag and code consolidation

#### Flag

Add `WAMR_BUILD_ZEPHYR_USERMODE_MT` to WAMR's build system, default 0.
When 1:
- `-DWAMR_BUILD_ZEPHYR_USERMODE_MT` is added to the compile
  definitions (so the platform layer can `#ifdef` on it).
- `zephyr_thread_usermode.c` is added to the Zephyr platform's source
  list.
- The `--undefined=z_impl_k_condvar_{init,signal,wait,broadcast}`
  linker hints are added (currently sample-side; move them platform-side
  so any consumer of the flag gets them for free).

When 0, the platform code is bit-for-bit equivalent to the pre-branch
state for everything that isn't the join-race fix.

#### Replace existing gating in `platform_internal.h` and `zephyr_thread.c`

Every existing `#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)`
in the two files becomes `#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT`. The
inside of each block is otherwise unchanged.

#### Unify the two thread-tracking lists in `zephyr_thread.c`

Replace `os_thread_obj` and `dyn_thread_node` with one node type:

```c
struct thread_obj_node {
    korp_tid tid;
    bool to_be_freed;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    bool is_dyn;        /* true → k_object_release(tid); false → BH_FREE(node) */
#endif
    struct thread_obj_node *next;
};
```

For kernel-mode allocations the node *is* the `os_thread_obj`
equivalent: it embeds `struct k_thread` at offset 0, `tid` aliases the
embedded thread, and reclaim does `BH_FREE(node)`. For dynamic
allocations the node holds only a `korp_tid` pointer and reclaim does
`k_object_release(tid); BH_FREE(node);`.

One list (`thread_obj_list`), one lock (`thread_obj_lock`), one reclaim
function (`thread_obj_list_reclaim`).

#### Split user-mode helpers into `zephyr_thread_usermode.c`

New file with the user-mode-only helpers — currently inside
`zephyr_thread.c` under `#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)`:
- `os_thread_env_init_for_usermode()`
- The dynamic-allocation branch of `os_thread_create_with_prio`
  (factored out into a helper `dyn_thread_alloc(...)` returning a
  `korp_tid` + `is_dyn` flag)
- The dynamic-cleanup branch of `thread_data_destroy` (factored into a
  helper `dyn_thread_release(...)`)
- The `dyn_thread_list_*` helpers are deleted, replaced by the unified
  list operations from `zephyr_thread.c`

`zephyr_thread.c` includes a small header declaring these helpers and
calls them inside the `#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT` branches.

#### Keep the join-race fix unconditional

`thread_exited` flag, the "check then wait, recheck under lock" pattern
in `os_thread_join`, and the deferred `thread_data_destroy` all stay in
`zephyr_thread.c` without any `#ifdef`. They benefit kernel-mode MT
consumers too and are why we need the upgraded `simple/` sample.

#### Condvar implementation

Keep both implementations gated by the flag (native `k_condvar` when
on, hand-rolled wait list when off). The native path is cleaner but
requires the linker hints, so leaving the legacy path for non-flag
builds keeps the zero-cost-when-off promise.

### Sub-project 2: Sample matrix

Three samples, each with a distinct purpose:

| Sample | Shape | Threading | Flag default | Toggle | Purpose |
|---|---|---|---|---|---|
| `simple/` | Zephyr-app | kernel-mode MT | off | — | Regression-tests the kernel-mode MT path (unified thread list, join-race fix) |
| `user-mode-app/` (new) | Zephyr-app | user-mode ST + bh_queue | on | flag on/off | Demonstrates flag necessity in app shape |
| `user-mode/` | Zephyr-library | user-mode MT or ST + bh_queue | on | `USER_MODE_MULTITHREAD` (ST/MT) + flag on/off | Library-shape coverage, all four combinations |

#### `simple/` upgrade

Rewrite `simple/src/main.c` to spawn 2 kernel-mode threads via
`os_thread_create`, each running a WASM instance, with `os_thread_join`
on both. Use `bh_queue` lightly to exercise the kernel-mode condvar
path (which under flag=off uses the legacy wait-list implementation).

The point: when the unified `thread_obj_node` refactor lands, a
list-corruption bug would silently pass with a single-thread `simple/`.
With MT, the second join hits the corrupted node and faults visibly.

#### `user-mode-app/` (new)

Layout copies `simple/` (Zephyr-app, `target_sources(app ...)`, no
`zephyr_library`, no `app_memory` partition). `prj.conf` has
`CONFIG_USERSPACE=y` and `CONFIG_DYNAMIC_OBJECTS=y`. CMakeLists sets
`WAMR_BUILD_ZEPHYR_USERMODE_MT=1` by default but the user can override
with `west build ... -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0`.

`src/main.c` spawns one user-mode thread that runs a trivial WASM app,
then does one `bh_post_msg` / `bh_get_msg` round trip. That single
bh_queue interaction is what forces the kernel slow-path on bh_queue's
internal mutex+condvar, triggering kobject validation:

- Flag on → kobjects allocated via `k_object_alloc`, registered,
  validation succeeds, demo completes.
- Flag off → kobjects in WAMR heap, unregistered, validation fails
  on the first condvar wait, board faults visibly with "address is
  not a known kernel object" or similar.

The README has both invocations and the expected output of each.

This is **manual-test guidance**, not a CI test. A CI job whose pass
condition is "board faults" would be a maintenance hazard. The CI side
runs only the flag-on configuration.

#### `user-mode/` refactor

Keep the directory and the existing CMakeLists/lib structure, but:

1. Add a sample-local `USER_MODE_MULTITHREAD` CMake option, default 1.
2. Add a CMake guard: if `USER_MODE_MULTITHREAD=1`, require
   `WAMR_BUILD_ZEPHYR_USERMODE_MT=1`, else `FATAL_ERROR`.
3. `lib-wamr-zephyr/wamr_lib.c` becomes one TU with two functions:
   `iwasm_main_st()` and `iwasm_main_mt()`, plus a tiny dispatcher
   `iwasm_main(...)` that picks one via `#ifdef USER_MODE_MULTITHREAD`.
4. `iwasm_main_mt` is the current branch's worker-pool + bh_queue
   demo (unchanged in behavior). `iwasm_main_st` is a single user-mode
   thread that runs the WASM app and does one bh_queue round trip
   (same pattern as `user-mode-app/`'s `main.c` body but in
   library-shape `wamr_lib.c`).

Cross-product behavior:

| `WAMR_BUILD_ZEPHYR_USERMODE_MT` | `USER_MODE_MULTITHREAD` | Outcome |
|---|---|---|
| 1 | 1 | Current branch behavior — works |
| 1 | 0 | ST + bh_queue, in library shape — works |
| 0 | 0 | ST + bh_queue, no dynamic kobjects → faults at first condvar wait |
| 0 | 1 | Rejected at CMake time |

#### Documentation moves

- `USERMODE.md` and `USERMODE_MULTITHREAD.md` currently sit in
  `product-mini/platforms/zephyr/user-mode/`. Fold their content into
  per-sample READMEs:
  - The platform-internals diagrams + "Fix 1..4" exposition stay (this
    is the reference for understanding why the flag exists). Move to
    `core/shared/platform/zephyr/README.md` or a top-level
    `docs/zephyr-usermode-internals.md` — they're platform-level, not
    sample-level.
  - The sample-specific portions (how to build, expected output, the
    flag-off failure mode) go in each sample's `README.md`.

### Sub-project 3: CMake / build-system plumbing

#### `WAMR_BUILD_ZEPHYR_USERMODE_MT` definition

Add to the canonical WAMR build options list (alongside `WAMR_BUILD_INTERP`,
`WAMR_BUILD_AOT`, etc.). When 1:
- Adds `-DWAMR_BUILD_ZEPHYR_USERMODE_MT` to `WAMR_COMPILE_OPTIONS`.
- Pulls `zephyr_thread_usermode.c` into the platform source list.
- Emits the linker `--undefined` hints via `zephyr_link_libraries` (or
  the appropriate platform-agnostic equivalent). This is currently done
  in the sample CMakeLists; relocating it to the platform layer means
  any consumer of the flag gets the correct link automatically.

#### CMake guard rails

- `user-mode/CMakeLists.txt` adds the `USER_MODE_MULTITHREAD=1 ⇒
  WAMR_BUILD_ZEPHYR_USERMODE_MT=1` check with `message(FATAL_ERROR ...)`.
- `user-mode-app/CMakeLists.txt` sets `WAMR_BUILD_ZEPHYR_USERMODE_MT=1`
  by default but allows override.
- `simple/CMakeLists.txt` leaves the flag at default (0).

## Trade-offs and alternatives considered

**Approach A (flag-only, no consolidation).** Smallest change. Just
swap the gate. Keeps duplicate code paths in `zephyr_thread.c`.
Rejected: the duplication is a maintenance hazard — two thread lists,
two reclaim functions, both with the same shape.

**Approach B (consolidation-only, keep auto-gate).** No new flag. Just
unify the lists and split out the helpers. Rejected: still drags
every `CONFIG_USERSPACE+CONFIG_DYNAMIC_OBJECTS` consumer onto the
dynamic-kobject path, which has nonzero cost (pointer-typed mutexes
add an indirection on every lock op, and the linker hints add link
time).

**Approach C (this spec).** Both. Most work, cleanest end state.

**Naming for the flag.** Considered `WAMR_BUILD_ZEPHYR_DYN_KOBJ` —
more accurate technically (it's about dynamic kobject allocation, not
multi-thread per se) but less obvious for users skimming options.
Picked `WAMR_BUILD_ZEPHYR_USERMODE_MT` because user-mode multi-thread
is the canonical use case and the macro shows up in code that's
clearly threading-related.

**Sample matrix shape.** Considered restoring the pre-branch
single-thread `user-mode/` as a sibling, plus a new `user-mode-mt/`.
Rejected: the ST/MT cross-product in one sample (controlled by
`USER_MODE_MULTITHREAD`) plus the flag toggle in `user-mode-app/`
gives the same coverage with one fewer directory to maintain.

## Acceptance criteria

1. `core/shared/platform/zephyr/zephyr_thread.c` has one thread-tracking
   list, one reclaim function. All user-mode-only allocation/release
   code lives in `zephyr_thread_usermode.c`.
2. All `#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)`
   gates in the platform layer are replaced with
   `#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT`.
3. With `WAMR_BUILD_ZEPHYR_USERMODE_MT=0`: platform code compiles
   identically (modulo the unconditional join-race fix) to the
   pre-branch state. Verified by running `simple/` and confirming
   identical output to `main`.
4. `simple/` builds and runs on qemu_x86 with 2 kernel-mode threads
   posting to a shared bh_queue. Both threads join cleanly.
5. `user-mode-app/` with the flag on: builds and runs the ST + bh_queue
   demo on qemu_x86 to completion. With the flag off: fails with a
   visible kobject-validation fault at the first condvar use, and the
   README documents the expected output.
6. `user-mode/` with `USER_MODE_MULTITHREAD=1` builds and runs on
   qemu_x86 with the same observable demo output (worker count, message
   sequence, completion message) as the current branch tip. With
   `USER_MODE_MULTITHREAD=0` and the flag on: builds and runs the ST +
   bh_queue demo. With the flag off and either ST or MT: CMake fails
   for MT, runtime faults for ST.
7. Documentation is reorganized: platform internals in
   `core/shared/platform/zephyr/`, per-sample READMEs cover only the
   sample-specific build/run/expected-output content.

## Out of scope (follow-ups)

- Zephyr-as-app + user-mode + multi-thread sample (one CMakeLists swap
  away if needed later).
- Adopting native `k_condvar` for kernel-mode builds (would let us
  retire the hand-rolled wait-list condvar entirely, but requires the
  linker hints unconditionally — separate decision).
- Twister test harness for the failure-mode demos. The faults are
  expected and documented, but a CI job that asserts on a board fault
  is fragile.

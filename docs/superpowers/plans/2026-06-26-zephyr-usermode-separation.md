# Zephyr User-Mode Multi-Thread: Platform Separation and Sample Matrix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the implicit `CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS` gate in WAMR's Zephyr platform layer with an explicit `WAMR_BUILD_ZEPHYR_USERMODE_MT` opt-in flag, consolidate the dual thread-tracking lists in `zephyr_thread.c`, split user-mode-only helpers into a new `zephyr_thread_usermode.c`, and rearrange the sample tree (`simple/`, `user-mode/`, new `user-mode-app/`) so each sample has a clear purpose with toggleable threading/flag modes.

**Architecture:** Single WAMR build flag controls the dynamic-kobject code path in both `platform_internal.h` and `zephyr_thread.c`. When off, the platform layer is bit-for-bit equivalent to the pre-branch state (apart from the unconditional join-race fix). When on, `zephyr_thread.c` calls into helpers in `zephyr_thread_usermode.c` for user-mode-specific allocation/release, and one unified `thread_obj_node` list replaces the previous parallel `os_thread_obj`/`dyn_thread_node` lists. Three samples cover the consumption matrix: `simple/` (Zephyr-app, kernel-mode MT), `user-mode/` (Zephyr-library, ST or MT, flag-toggleable), `user-mode-app/` (Zephyr-app, user-mode ST + bh_queue, flag-toggleable to demonstrate flag necessity).

**Tech Stack:** C99, Zephyr RTOS (≥ 3.2.0 namespaced kernel headers, ≥ 4.0.0 also tested), CMake (Zephyr's `find_package(Zephyr)` build), qemu_x86 for verification.

## Global Constraints

- **Flag name:** `WAMR_BUILD_ZEPHYR_USERMODE_MT`. Default `0`. Set to `1` only by samples that need user-mode multi-thread or user-mode + bh_queue/condvar.
- **Sample-local flag:** `USER_MODE_MULTITHREAD` inside `user-mode/`. Default `1`. When `1` requires `WAMR_BUILD_ZEPHYR_USERMODE_MT=1` (CMake `FATAL_ERROR` otherwise).
- **Join-race fix is unconditional:** `thread_exited` flag and the check-recheck join pattern stay in `zephyr_thread.c` without any `#ifdef`. They benefit kernel-mode MT too.
- **Zero-cost-when-off:** With `WAMR_BUILD_ZEPHYR_USERMODE_MT=0`, the platform layer must produce code semantically equivalent to the pre-branch (`main`) state for all paths except the join-race fix. `simple/` (with the flag off) must build and run cleanly.
- **Linker `--undefined` hints:** Currently emitted from the sample's `lib-wamr-zephyr/CMakeLists.txt`. Move them to the platform-side `shared_platform.cmake` so any consumer of the flag inherits them automatically.
- **License headers preserved:** `SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)` stays on `zephyr_thread.c` and `platform_internal.h`. The new `zephyr_thread_usermode.c` gets the same Siemens header since it carries the same code.
- **Documentation policy:** Don't create new top-level markdown files unless explicitly mandated by a task. Per-sample READMEs are existing files; folding old `USERMODE*.md` content into them is allowed.
- **`docs/` is in local `.gitignore`:** Use `git add -f` when staging any new file under `docs/`.
- **Comments policy:** Only write comments where the *why* is non-obvious (e.g., the kobject-validation subtlety, the linker `--undefined` workaround). Don't narrate what code does.
- **Commit cadence:** One commit per task. Co-author footer: `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `core/shared/platform/zephyr/platform_internal.h` | Modify | Replace `CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS` gates with `WAMR_BUILD_ZEPHYR_USERMODE_MT` |
| `core/shared/platform/zephyr/zephyr_thread.c` | Modify | Replace gates; collapse dual thread-tracking lists into one `thread_obj_node`; delegate user-mode-only ops to `zephyr_thread_usermode.c` |
| `core/shared/platform/zephyr/zephyr_thread_internal.h` | Create | Private header declaring helpers shared between `zephyr_thread.c` and `zephyr_thread_usermode.c` |
| `core/shared/platform/zephyr/zephyr_thread_usermode.c` | Create | User-mode-only helpers: `os_thread_env_init_for_usermode`, `dyn_thread_alloc`, `dyn_thread_release`, dynamic kobject paths |
| `core/shared/platform/zephyr/shared_platform.cmake` | Modify | Add the linker `--undefined` hints when flag is on; exclude `zephyr_thread_usermode.c` from build when flag is off |
| `build-scripts/config_common.cmake` | Modify | Register `WAMR_BUILD_ZEPHYR_USERMODE_MT` option, emit `add_definitions` and status message |
| `product-mini/platforms/zephyr/simple/src/main.c` | Modify | Upgrade to kernel-mode MT (2 threads via `os_thread_create`, bh_queue round trip) |
| `product-mini/platforms/zephyr/simple/CMakeLists.txt` | Modify | Enable `WAMR_BUILD_THREAD_MGR` (already needed for `os_thread_create` from external code) and keep flag at default 0 |
| `product-mini/platforms/zephyr/user-mode/CMakeLists.txt` | Modify | Add `USER_MODE_MULTITHREAD` option and consistency check with `WAMR_BUILD_ZEPHYR_USERMODE_MT` |
| `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/CMakeLists.txt` | Modify | Drop sample-side linker `--undefined` hints (moved platform-side); pass `USER_MODE_MULTITHREAD` to source compilation |
| `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/wamr_lib.c` | Modify | One TU, two function bodies (`iwasm_main_st`, `iwasm_main_mt`), dispatcher chooses via `#ifdef USER_MODE_MULTITHREAD` |
| `product-mini/platforms/zephyr/user-mode/README.md` | Modify | Update for ST/MT toggle and flag toggle; document expected output for both modes |
| `product-mini/platforms/zephyr/user-mode-app/` | Create | New sample: Zephyr-app shape, user-mode single-thread, demonstrates flag necessity |
| `product-mini/platforms/zephyr/user-mode-app/CMakeLists.txt` | Create | Sets `WAMR_BUILD_ZEPHYR_USERMODE_MT=1` by default, overridable via `-D` |
| `product-mini/platforms/zephyr/user-mode-app/prj.conf` | Create | `CONFIG_USERSPACE=y`, `CONFIG_DYNAMIC_OBJECTS=y`, `CONFIG_HEAP_MEM_POOL_SIZE=4096` |
| `product-mini/platforms/zephyr/user-mode-app/src/main.c` | Create | Kernel `main()` spawns one user-mode thread; user-mode thread does WASM + one bh_queue round trip |
| `product-mini/platforms/zephyr/user-mode-app/README.md` | Create | Build/run instructions for both flag values, expected output of each |
| `docs/zephyr-usermode-internals.md` | Create | Platform-internals reference: kobject registration gap, `K_INHERIT_PERMS`, linker `--undefined`, join race. Merges the persistent content from `USERMODE.md` and `USERMODE_MULTITHREAD.md` |
| `product-mini/platforms/zephyr/user-mode/USERMODE.md` | Delete (after content move) | Content split between platform internals doc and per-sample README |
| `product-mini/platforms/zephyr/user-mode/USERMODE_MULTITHREAD.md` | Delete (after content move) | Same as above |

## Task Ordering Rationale

Platform-layer tasks run first (1–6) so that all sample tasks build against the final platform API. Sample tasks (7–11) then run in any order. Documentation and cleanup (12–13) close out.

---

### Task 1: Register `WAMR_BUILD_ZEPHYR_USERMODE_MT` build option

**Files:**
- Modify: `build-scripts/config_common.cmake:389` (add option block near end of the status-message block)

**Interfaces:**
- Consumes: nothing (this is the root of the flag)
- Produces: `WAMR_BUILD_ZEPHYR_USERMODE_MT` CMake variable; `-DWAMR_BUILD_ZEPHYR_USERMODE_MT=1` compile definition when set

- [ ] **Step 1: Read the surrounding context in `config_common.cmake`**

Run: `grep -n "WAMR_BUILD_LIB_RATS\|LIB_RATS" /home/tl/projects/wasm-micro-runtime/build-scripts/config_common.cmake | tail -20`

Expected: locate the existing pattern where a `WAMR_BUILD_*` flag is checked, `add_definitions` is called, and a `message(...)` is emitted. We'll mirror that pattern.

- [ ] **Step 2: Append the new option block at the end of `config_common.cmake`**

Find the last `endif ()` in the file and insert before it (or at end-of-file — context-dependent, both fine). Use exact text:

```cmake
if (WAMR_BUILD_ZEPHYR_USERMODE_MT EQUAL 1)
  if (NOT WAMR_BUILD_PLATFORM STREQUAL "zephyr")
    message (FATAL_ERROR
      "WAMR_BUILD_ZEPHYR_USERMODE_MT is only valid on the zephyr platform")
  endif ()
  add_definitions (-DWAMR_BUILD_ZEPHYR_USERMODE_MT=1)
  message ("     Zephyr user-mode multi-thread enabled (dynamic kobject allocation)")
endif ()
```

- [ ] **Step 3: Verify it parses by running CMake on `simple/` with the flag off**

Run from a Zephyr-ready shell (or skip if no Zephyr SDK is present — see verification notes in Task 7):

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/simple
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0
```

Expected: build completes without referencing the new flag in the output. Status message `Zephyr user-mode multi-thread enabled` must NOT appear.

If Zephyr SDK is unavailable, syntax-check via CMake:

```bash
cmake -P /home/tl/projects/wasm-micro-runtime/build-scripts/config_common.cmake 2>&1 | head -5
```

Expected: parses without syntax errors (CMake may report missing variables, that's fine — the file is normally `include()`-d, not run standalone).

- [ ] **Step 4: Commit**

```bash
git add build-scripts/config_common.cmake
git commit -m "$(cat <<'EOF'
build: register WAMR_BUILD_ZEPHYR_USERMODE_MT option

New WAMR-side opt-in flag for the Zephyr user-mode multi-thread code
path. When 1, defines -DWAMR_BUILD_ZEPHYR_USERMODE_MT=1 for the platform
layer and emits a status message. Rejects use on non-Zephyr platforms.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Create `zephyr_thread_internal.h` with the shared helper declarations

**Files:**
- Create: `core/shared/platform/zephyr/zephyr_thread_internal.h`

**Interfaces:**
- Consumes: `korp_tid` type from `platform_internal.h`
- Produces: the helper function signatures used by `zephyr_thread.c` ↔ `zephyr_thread_usermode.c`

- [ ] **Step 1: Create the header**

Exact file contents:

```c
/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _ZEPHYR_THREAD_INTERNAL_H
#define _ZEPHYR_THREAD_INTERNAL_H

#include "platform_api_vmcore.h"

#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT

/*
 * Allocate a k_thread via k_object_alloc so it is registered in the
 * kernel object table for syscall validation when the parent is a
 * user-mode thread. Returns NULL on failure.
 */
korp_tid dyn_thread_alloc(void);

/*
 * Release a k_thread previously allocated by dyn_thread_alloc().
 * Safe to call from user mode (uses k_object_release, which is a syscall).
 */
void dyn_thread_release(korp_tid tid);

#endif /* WAMR_BUILD_ZEPHYR_USERMODE_MT */

#endif /* _ZEPHYR_THREAD_INTERNAL_H */
```

- [ ] **Step 2: Verify the file exists and the include guard is correct**

Run: `head -20 /home/tl/projects/wasm-micro-runtime/core/shared/platform/zephyr/zephyr_thread_internal.h`

Expected: header block + `#ifndef _ZEPHYR_THREAD_INTERNAL_H` matches.

- [ ] **Step 3: Commit**

```bash
git add core/shared/platform/zephyr/zephyr_thread_internal.h
git commit -m "$(cat <<'EOF'
platform/zephyr: add zephyr_thread_internal.h for shared helpers

Private header declaring dyn_thread_alloc / dyn_thread_release, which
are defined in the new zephyr_thread_usermode.c (next commit) and
called from zephyr_thread.c. Gated by WAMR_BUILD_ZEPHYR_USERMODE_MT.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Create `zephyr_thread_usermode.c` carrying the user-mode-only helpers

**Files:**
- Create: `core/shared/platform/zephyr/zephyr_thread_usermode.c`

**Interfaces:**
- Consumes: `korp_tid` from `platform_internal.h`; `BH_ENABLE_ZEPHYR_MPU_STACK`, `BH_ZEPHYR_MPU_STACK_COUNT`, `mpu_stacks` from `zephyr_thread.c` (extern-declared here)
- Produces: implementations of `dyn_thread_alloc`, `dyn_thread_release`, `os_thread_env_init_for_usermode`

- [ ] **Step 1: Create the file**

Exact file contents:

```c
/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "zephyr_thread_internal.h"

#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
extern struct z_thread_stack_element
    mpu_stacks[BH_ZEPHYR_MPU_STACK_COUNT][BH_ZEPHYR_MPU_STACK_SIZE_ELEMENTS];
#endif

korp_tid
dyn_thread_alloc(void)
{
    return (korp_tid)k_object_alloc(K_OBJ_THREAD);
}

void
dyn_thread_release(korp_tid tid)
{
    if (tid)
        k_object_release(tid);
}

/*
 * MPU stacks (mpu_stacks[]) are static kernel objects. k_thread_create
 * validates that the *calling* thread has permission to the stack
 * object being passed — not just the child. A user-mode thread can't
 * grant itself access to stacks it doesn't own yet, so this must be
 * called from supervisor mode before the user-mode thread starts.
 */
void
os_thread_env_init_for_usermode(k_tid_t tid)
{
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    int i;
    for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
        k_object_access_grant(mpu_stacks[i], tid);
    }
#else
    (void)tid;
#endif
}

#endif /* WAMR_BUILD_ZEPHYR_USERMODE_MT */
```

NOTE: the exact extern-declaration of `mpu_stacks` depends on the macro that `K_THREAD_STACK_ARRAY_DEFINE` expands to. Verify with:

```bash
grep -A 5 "K_THREAD_STACK_ARRAY_DEFINE" /home/tl/projects/wasm-micro-runtime/core/shared/platform/zephyr/zephyr_thread.c
```

If the extern-declaration above is incorrect, replace it with the appropriate Zephyr macro (the cleanest portable approach is to keep `mpu_stacks` static in `zephyr_thread.c` and instead expose a helper there — e.g., `mpu_stack_addr(int i)` — that returns the stack address. If the extern doesn't compile, switch to that pattern: declare `extern void *mpu_stack_addr(int i);` here and define it in `zephyr_thread.c`.) See Task 4 for the matching change.

- [ ] **Step 2: (decision) Verify which `mpu_stacks` access pattern compiles**

If you can build Zephyr: try the extern approach first; if it errors with an unknown type, switch to the `mpu_stack_addr(i)` helper pattern described in Step 1.

If you can't build Zephyr: pick the safer `mpu_stack_addr(i)` helper pattern up front. Adjust this file to:

```c
extern void *mpu_stack_addr(int i);

void
os_thread_env_init_for_usermode(k_tid_t tid)
{
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    int i;
    for (i = 0; i < BH_ZEPHYR_MPU_STACK_COUNT; i++) {
        void *p = mpu_stack_addr(i);
        if (p)
            k_object_access_grant(p, tid);
    }
#else
    (void)tid;
#endif
}
```

Record the choice in the commit message so Task 4 can match it.

- [ ] **Step 3: Commit**

```bash
git add core/shared/platform/zephyr/zephyr_thread_usermode.c
git commit -m "$(cat <<'EOF'
platform/zephyr: add zephyr_thread_usermode.c

Split user-mode-only helpers out of zephyr_thread.c. This file is
only compiled when WAMR_BUILD_ZEPHYR_USERMODE_MT=1. It owns:

  - dyn_thread_alloc: k_object_alloc(K_OBJ_THREAD)
  - dyn_thread_release: k_object_release
  - os_thread_env_init_for_usermode: grant MPU stack access (resolves
    the chicken-and-egg between user-mode parent and child thread
    creation)

zephyr_thread.c (next commit) calls these via zephyr_thread_internal.h.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Refactor `zephyr_thread.c` — unified list, gate replacement, delegation

**Files:**
- Modify: `core/shared/platform/zephyr/zephyr_thread.c`

**Interfaces:**
- Consumes: `dyn_thread_alloc`, `dyn_thread_release` from `zephyr_thread_internal.h`
- Produces: `struct thread_obj_node` (private to this TU); the public `os_thread_create*`, `os_thread_join`, etc. APIs continue to work as before

- [ ] **Step 1: Replace gate macros throughout the file**

Search and replace every `#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)` with `#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT`. Mirror endif comments. There are ~10 occurrences in `zephyr_thread.c`.

Verify: `grep -nc 'CONFIG_USERSPACE.*CONFIG_DYNAMIC_OBJECTS' core/shared/platform/zephyr/zephyr_thread.c` returns `0`.

- [ ] **Step 2: Add include for the new internal header**

Near the top of `zephyr_thread.c`, after the existing includes:

```c
#include "zephyr_thread_internal.h"
```

- [ ] **Step 3: Replace the two thread-tracking structs with the unified one**

Locate (currently in `zephyr_thread.c` around lines ~98–104 and 250–319):

```c
typedef struct os_thread_obj {
    struct k_thread thread;
    bool to_be_freed;
    struct os_thread_obj *next;
} os_thread_obj;
```

and:

```c
struct dyn_thread_node { ... };
static struct dyn_thread_node *dyn_thread_list = NULL;
static void dyn_thread_list_add(korp_tid tid) { ... }
static void dyn_thread_mark_freed(korp_tid tid) { ... }
static void dyn_thread_list_reclaim(void) { ... }
```

Replace both with one unified type and one list. Exact replacement:

```c
typedef struct thread_obj_node {
    /* Kernel-mode path: this node IS the k_thread storage (offset 0).
     * User-mode path: this node points to a separately-allocated k_thread. */
    union {
        struct k_thread thread;        /* used when is_dyn == false */
        korp_tid dyn_tid;              /* used when is_dyn == true */
    };
    bool to_be_freed;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    bool is_dyn;
#endif
    struct thread_obj_node *next;
} thread_obj_node;

static thread_obj_node *thread_obj_list = NULL;
```

Replace `static os_thread_obj *thread_obj_list` and delete `static struct dyn_thread_node *dyn_thread_list`.

NOTE: this changes the storage layout for the kernel-mode path — `&node->thread` (the embedded k_thread, accessed via the union) replaces `&node->thread` (previously a plain field). Old call sites pass `tid = (korp_tid)BH_MALLOC(sizeof(os_thread_obj))` and treat it directly as `k_tid_t`. Under the new layout, kernel-mode allocation becomes:

```c
thread_obj_node *node = BH_MALLOC(sizeof(thread_obj_node));
memset(node, 0, sizeof(*node));
tid = (korp_tid)&node->thread;
```

The cast `tid = &node->thread` works because the union puts `struct k_thread` at offset 0 of the node, so `(thread_obj_node *)tid == node` round-trips.

- [ ] **Step 4: Unify the reclaim function**

Replace the body of `thread_obj_list_reclaim` and delete `dyn_thread_list_reclaim`. New body:

```c
static void
thread_obj_list_reclaim(void)
{
    thread_obj_node *p, *p_prev;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    p_prev = NULL;
    p = thread_obj_list;
    while (p) {
        if (p->to_be_freed) {
            thread_obj_node *next = p->next;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
            if (p->is_dyn)
                dyn_thread_release(p->dyn_tid);
#endif
            BH_FREE(p);
            if (p_prev == NULL)
                thread_obj_list = next;
            else
                p_prev->next = next;
            p = next;
        }
        else {
            p_prev = p;
            p = p->next;
        }
    }
    zmutex_unlock(&thread_obj_lock);
}
```

- [ ] **Step 5: Rewrite `os_thread_create_with_prio` to use the unified path**

Replace the existing two-branch allocation block (currently `BH_MALLOC` vs. `k_object_alloc` switched on `is_user_mode`) with:

```c
int
os_thread_create_with_prio(korp_tid *p_tid, thread_start_routine_t start,
                           void *arg, unsigned int stack_size, int prio)
{
    korp_tid tid;
    thread_obj_node *node;
    os_thread_data *thread_data;
    unsigned thread_data_size;
    int options = 0;
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    bool is_user_mode = k_is_user_context();
#endif

    if (!p_tid || !stack_size)
        return BHT_ERROR;

    thread_obj_list_reclaim();

    if (!(node = BH_MALLOC(sizeof(thread_obj_node))))
        return BHT_ERROR;
    memset(node, 0, sizeof(*node));

#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (is_user_mode) {
        node->is_dyn = true;
        if (!(node->dyn_tid = dyn_thread_alloc())) {
            BH_FREE(node);
            return BHT_ERROR;
        }
        tid = node->dyn_tid;
        options = K_USER | K_INHERIT_PERMS;
    }
    else
#endif
    {
        tid = (korp_tid)&node->thread;
    }

    /* ... existing thread_data allocation, MPU stack alloc, k_thread_create
     * call exactly as before, with `tid` already set above ... */

    /* On success path, add node to the unified list: */
    thread_obj_list_add_node(node);
    *p_tid = tid;
    return BHT_OK;

fail3:
    /* ... existing fail labels, but instead of:
     *   BH_FREE(tid); or k_object_release(tid);
     * replace with:
     */
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    if (node->is_dyn)
        dyn_thread_release(node->dyn_tid);
#endif
    BH_FREE(node);
    return BHT_ERROR;
}
```

Add a small helper `thread_obj_list_add_node(thread_obj_node *node)` that takes the unified node type (replacing the old `thread_obj_list_add(os_thread_obj *)`).

Delete `dyn_thread_list_add` and `dyn_thread_mark_freed`.

- [ ] **Step 6: Update `thread_data_destroy` to mark the unified node**

The existing function (around line 173):

```c
static void
thread_data_destroy(os_thread_data *thread_data)
{
    thread_data_list_remove(thread_data);
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
    if (k_is_user_context())
        dyn_thread_mark_freed(thread_data->tid);
    else
#endif
        ((os_thread_obj *)thread_data->tid)->to_be_freed = true;
    ...
}
```

becomes:

```c
static void
thread_data_destroy(os_thread_data *thread_data)
{
    thread_data_list_remove(thread_data);
    /* Mark the matching node for deferred reclaim. Works for both
     * kernel-mode (tid is &node->thread) and user-mode (we look up
     * the node by its dyn_tid). */
    thread_obj_list_mark_freed(thread_data->tid);
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
    mpu_stack_free(thread_data->stack);
#endif
    BH_FREE(thread_data);
}
```

Add a helper:

```c
static void
thread_obj_list_mark_freed(korp_tid tid)
{
    thread_obj_node *p;
    zmutex_lock(&thread_obj_lock, K_FOREVER);
    p = thread_obj_list;
    while (p) {
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
        bool match = p->is_dyn ? (p->dyn_tid == tid)
                               : ((korp_tid)&p->thread == tid);
#else
        bool match = ((korp_tid)&p->thread == tid);
#endif
        if (match) {
            p->to_be_freed = true;
            break;
        }
        p = p->next;
    }
    zmutex_unlock(&thread_obj_lock);
}
```

- [ ] **Step 7: If Task 3 chose the `mpu_stack_addr(i)` helper pattern, add the definition here**

If you went with the helper pattern (rather than `extern struct z_thread_stack_element ...`), add to `zephyr_thread.c`:

```c
#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
void *
mpu_stack_addr(int i)
{
    if (i < 0 || i >= BH_ZEPHYR_MPU_STACK_COUNT)
        return NULL;
    return (void *)mpu_stacks[i];
}
#endif
```

If you went with the extern pattern in Task 3, do nothing here.

- [ ] **Step 8: Delete `os_thread_env_init_for_usermode` from this file**

It now lives in `zephyr_thread_usermode.c`. Remove the `#if defined(CONFIG_USERSPACE) ... void os_thread_env_init_for_usermode(...) { ... } #endif` block (was lines 351–380 pre-edit).

- [ ] **Step 9: Verify no orphaned references remain**

Run:

```bash
grep -nE 'os_thread_obj|dyn_thread_node|dyn_thread_list_(add|mark_freed|reclaim)' core/shared/platform/zephyr/zephyr_thread.c
```

Expected: no matches. Every reference to the old types/functions should be gone.

```bash
grep -nE 'CONFIG_USERSPACE.*CONFIG_DYNAMIC_OBJECTS' core/shared/platform/zephyr/zephyr_thread.c
```

Expected: no matches.

- [ ] **Step 10: Build verification — flag off**

If Zephyr SDK is available:

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/simple
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -40
```

Expected: pre-existing simple/ output ("Hello world!" from WASM, "elapsed: <ms>").

If not available: at minimum verify compile via:

```bash
gcc -fsyntax-only -I core/shared/platform/include -I core/shared/platform/zephyr -I core/iwasm/include core/shared/platform/zephyr/zephyr_thread.c 2>&1 | head -20
```

(Expected: most errors are about missing Zephyr headers — those are fine. Verify no errors of the form "use of undeclared identifier 'os_thread_obj'", "use of undeclared identifier 'dyn_thread_node'", etc.)

- [ ] **Step 11: Commit**

```bash
git add core/shared/platform/zephyr/zephyr_thread.c
git commit -m "$(cat <<'EOF'
platform/zephyr: unify thread-tracking lists, gate on WAMR_BUILD_ZEPHYR_USERMODE_MT

Three structural changes:

  1. Replace #if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
     gates with #ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT. The flag is a WAMR
     build-time opt-in, not derived from Zephyr Kconfigs.

  2. Collapse os_thread_obj and dyn_thread_node into one thread_obj_node
     type with a union (embedded k_thread for kernel mode / pointer to
     k_object_alloc'd thread for user mode) and an is_dyn discriminator.
     One list, one reclaim function.

  3. Delegate user-mode-only operations (dyn_thread_alloc,
     dyn_thread_release, os_thread_env_init_for_usermode) to the new
     zephyr_thread_usermode.c via zephyr_thread_internal.h.

The join-race fix from the prior commit stays unconditional.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Replace gates in `platform_internal.h`

**Files:**
- Modify: `core/shared/platform/zephyr/platform_internal.h:134, 181, 196, 213`

**Interfaces:**
- Consumes: `WAMR_BUILD_ZEPHYR_USERMODE_MT` definition from Task 1
- Produces: `zmutex_t`, `zsem_t`, `korp_cond` macros gated on the new flag

- [ ] **Step 1: Replace the gate at line 134**

Old:
```c
#if defined(CONFIG_USERSPACE) && defined(CONFIG_DYNAMIC_OBJECTS)
```

New:
```c
#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
```

Apply at every occurrence in `platform_internal.h` (lines 134, 181, 196, 213 pre-edit — verify exact line numbers with `grep -n 'CONFIG_USERSPACE.*CONFIG_DYNAMIC_OBJECTS' core/shared/platform/zephyr/platform_internal.h`).

The matching `#else /* !CONFIG_USERSPACE || !CONFIG_DYNAMIC_OBJECTS */` becomes `#else /* !WAMR_BUILD_ZEPHYR_USERMODE_MT */` and the closing `#endif /* CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS */` becomes `#endif /* WAMR_BUILD_ZEPHYR_USERMODE_MT */`.

- [ ] **Step 2: Verify no orphaned gates remain**

```bash
grep -n 'CONFIG_USERSPACE.*CONFIG_DYNAMIC_OBJECTS' core/shared/platform/zephyr/platform_internal.h
```

Expected: no matches.

- [ ] **Step 3: Commit**

```bash
git add core/shared/platform/zephyr/platform_internal.h
git commit -m "$(cat <<'EOF'
platform/zephyr: gate platform_internal.h on WAMR_BUILD_ZEPHYR_USERMODE_MT

Replace CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS gates around the
pointer-typed zmutex_t/zsem_t and the k_condvar-backed korp_cond with
the explicit WAMR opt-in flag. Behavior unchanged when flag is on;
when off, types revert to pre-branch (struct k_mutex / struct k_sem
value types, hand-rolled wait-list condvar).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Move linker `--undefined` hints to `shared_platform.cmake`, add `zephyr_thread_usermode.c` to source list

**Files:**
- Modify: `core/shared/platform/zephyr/shared_platform.cmake`

**Interfaces:**
- Consumes: `WAMR_BUILD_ZEPHYR_USERMODE_MT` from Task 1
- Produces: platform-side linker hints; conditional inclusion of `zephyr_thread_usermode.c` in the platform source list

- [ ] **Step 1: Read the current file**

(Already done — 28 lines. The `file (GLOB_RECURSE source_all ${PLATFORM_SHARED_DIR}/*.c)` picks up every `.c` file in the directory, including `zephyr_thread_usermode.c`. We need to remove that file from the list when the flag is off.)

- [ ] **Step 2: Append source-list filter and linker hints**

After the existing `list(REMOVE_ITEM ...)` block (around line 21), add:

```cmake
if (NOT WAMR_BUILD_ZEPHYR_USERMODE_MT EQUAL 1)
    list(REMOVE_ITEM source_all ${PLATFORM_SHARED_DIR}/zephyr_thread_usermode.c)
endif ()

if (WAMR_BUILD_ZEPHYR_USERMODE_MT EQUAL 1)
    # Under user-mode multi-thread, WAMR uses k_condvar via k_object_alloc.
    # Nothing in Zephyr's --whole-archive code references condvar symbols,
    # so the linker skips them when scanning libkernel.a in its single
    # pass. --undefined pre-marks the symbols as needed so libkernel.a
    # extraction picks them up before reaching wamr_lib.a.
    if (DEFINED ZEPHYR_BASE)
        zephyr_link_libraries(
            -Wl,--undefined=z_impl_k_condvar_init
            -Wl,--undefined=z_impl_k_condvar_signal
            -Wl,--undefined=z_impl_k_condvar_wait
            -Wl,--undefined=z_impl_k_condvar_broadcast
        )
    endif ()
endif ()
```

The `if (DEFINED ZEPHYR_BASE)` guard ensures the `zephyr_link_libraries` call is only invoked inside a real Zephyr build (where that function is defined) — protects standalone WAMR builds that don't have it.

- [ ] **Step 3: Verify the file parses**

```bash
cmake -P /home/tl/projects/wasm-micro-runtime/core/shared/platform/zephyr/shared_platform.cmake 2>&1 | head -10
```

Expected: parses; complaints about undefined variables (`WAMR_BUILD_LIBC_WASI`, `ZEPHYR_BASE`) are expected for standalone parsing — what we care about is no *syntax* errors.

- [ ] **Step 4: Commit**

```bash
git add core/shared/platform/zephyr/shared_platform.cmake
git commit -m "$(cat <<'EOF'
platform/zephyr: gate zephyr_thread_usermode.c and linker hints on flag

shared_platform.cmake:
  - Drop zephyr_thread_usermode.c from the source list when
    WAMR_BUILD_ZEPHYR_USERMODE_MT != 1 (parallels the existing
    socket/file/clock filtering for WAMR_BUILD_LIBC_WASI).
  - When the flag is on, emit --undefined=z_impl_k_condvar_* linker
    hints via zephyr_link_libraries so the linker extracts condvar
    syscall implementations from libkernel.a.

These hints were previously in the user-mode sample's CMakeLists; moving
them platform-side means any consumer of the flag inherits them.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Upgrade `simple/` to kernel-mode multi-thread

**Files:**
- Modify: `product-mini/platforms/zephyr/simple/src/main.c` (replace single-thread `iwasm_main` body and `iwasm_init` with a 2-thread + bh_queue demo)
- Modify: `product-mini/platforms/zephyr/simple/CMakeLists.txt:23` (ensure `WAMR_BUILD_THREAD_MGR=1`)

**Interfaces:**
- Consumes: `os_thread_create`, `os_thread_join` from the refactored platform; `bh_queue_create`, `bh_post_msg`, `bh_get_msg` from `core/shared/utils/bh_queue.h`
- Produces: a runnable `simple/` sample that spawns 2 kernel-mode worker threads, sends/receives 4 messages total via bh_queue, and exits cleanly

- [ ] **Step 1: Add `WAMR_BUILD_THREAD_MGR=1` to `simple/CMakeLists.txt`**

After the existing `WAMR_BUILD_LIBC_WASI` block (around line 38), add:

```cmake
if (NOT DEFINED WAMR_BUILD_THREAD_MGR)
  set (WAMR_BUILD_THREAD_MGR 1)
endif ()
```

- [ ] **Step 2: Rewrite `simple/src/main.c` body**

Replace the existing `iwasm_main`, `app_instance_main`, and `iwasm_init` functions (lines 47–186) with the multi-thread demo. Keep the includes, `CONFIG_*` size macros, and `global_heap_buf` definition.

New `iwasm_main`:

```c
struct test_msg {
    int worker_id;
    int seq;
};

#define MSGS_PER_WORKER 2
#define NUM_WORKERS 2

struct worker_ctx {
    int id;
    bh_queue *queue;
    wasm_module_t module;
};

static void *
worker_entry(void *arg)
{
    struct worker_ctx *ctx = (struct worker_ctx *)arg;
    wasm_module_inst_t inst;
    char error_buf[128];
    int i;

    inst = wasm_runtime_instantiate(ctx->module, CONFIG_APP_STACK_SIZE,
                                    CONFIG_APP_HEAP_SIZE, error_buf,
                                    sizeof(error_buf));
    if (!inst) {
        printf("  worker %d: instantiate failed: %s\n", ctx->id, error_buf);
        return NULL;
    }

    wasm_application_execute_main(inst, 0, NULL);

    for (i = 0; i < MSGS_PER_WORKER; i++) {
        struct test_msg *msg = BH_MALLOC(sizeof(struct test_msg));
        if (!msg)
            break;
        msg->worker_id = ctx->id;
        msg->seq = i;
        if (!bh_post_msg(ctx->queue, 0, msg, sizeof(struct test_msg))) {
            BH_FREE(msg);
            break;
        }
    }

    wasm_runtime_deinstantiate(inst);
    return NULL;
}

void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    bh_queue *queue = NULL;
    char error_buf[128];
    korp_tid worker_tids[NUM_WORKERS] = { 0 };
    struct worker_ctx worker_ctxs[NUM_WORKERS];
    int i, received = 0;

    (void)arg1; (void)arg2; (void)arg3;

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
#else
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
#endif
    if (!wasm_runtime_full_init(&init_args)) {
        printf("WAMR init failed\n");
        return;
    }

    module = wasm_runtime_load((uint8 *)wasm_test_file, sizeof(wasm_test_file),
                               error_buf, sizeof(error_buf));
    if (!module) {
        printf("Load failed: %s\n", error_buf);
        goto cleanup;
    }

    queue = bh_queue_create();
    if (!queue) {
        printf("Queue create failed\n");
        goto cleanup;
    }

    printf("=== simple kernel-mode MT demo: %d workers x %d msgs ===\n",
           NUM_WORKERS, MSGS_PER_WORKER);

    for (i = 0; i < NUM_WORKERS; i++) {
        worker_ctxs[i].id = i;
        worker_ctxs[i].queue = queue;
        worker_ctxs[i].module = module;
        if (os_thread_create(&worker_tids[i], worker_entry, &worker_ctxs[i],
                             CONFIG_APP_STACK_SIZE) != BHT_OK) {
            printf("Failed to create worker %d\n", i);
            break;
        }
    }

    while (received < NUM_WORKERS * MSGS_PER_WORKER) {
        bh_message_t bmsg = bh_get_msg(queue, BHT_WAIT_FOREVER);
        if (!bmsg)
            continue;
        struct test_msg *m = (struct test_msg *)bh_message_payload(bmsg);
        printf("  [recv] worker %d seq %d\n", m->worker_id, m->seq);
        BH_FREE(m);
        bh_free_msg(bmsg);
        received++;
    }

    for (i = 0; i < NUM_WORKERS; i++) {
        if (worker_tids[i])
            os_thread_join(worker_tids[i], NULL);
    }

    printf("=== simple demo complete: received %d msgs ===\n", received);

cleanup:
    if (queue)
        bh_queue_destroy(queue);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
}
```

Keep `iwasm_init` and the `main()` boilerplate unchanged — the existing supervisor-mode `k_thread_create(... iwasm_main, ..., 0, K_NO_WAIT)` works for the new body since it still runs in kernel mode.

Add `#include "bh_queue.h"` near the existing `#include "bh_platform.h"`.

- [ ] **Step 3: Build and run on qemu_x86**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/simple
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -60
```

Expected output (worker order/interleave will vary):

```
*** Booting Zephyr OS build ... ***
=== simple kernel-mode MT demo: 2 workers x 2 msgs ===
Hello world!
buf ptr: 0x1458
buf: 1234
Hello world!
buf ptr: 0x1458
buf: 1234
  [recv] worker 0 seq 0
  [recv] worker 1 seq 0
  [recv] worker 0 seq 1
  [recv] worker 1 seq 1
=== simple demo complete: received 4 msgs ===
```

- [ ] **Step 4: Commit**

```bash
git add product-mini/platforms/zephyr/simple/src/main.c \
        product-mini/platforms/zephyr/simple/CMakeLists.txt
git commit -m "$(cat <<'EOF'
sample/zephyr/simple: upgrade to kernel-mode multi-thread

Replace the single-thread "hello + elapsed" demo with a 2-worker
bh_queue round trip:
  - kernel main spawns 2 workers via os_thread_create
  - each worker instantiates the WASM module and runs it
  - workers post 2 messages each to a shared bh_queue
  - main consumes via bh_get_msg, then joins both workers

Exercises the unified thread_obj_node list and the join-race fix that
landed in the platform refactor. Without flag=on, this runs the legacy
condvar wait-list path; with flag=on (sample doesn't set it), it'd use
native k_condvar.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Add `USER_MODE_MULTITHREAD` toggle to `user-mode/`, restructure `wamr_lib.c`

**Files:**
- Modify: `product-mini/platforms/zephyr/user-mode/CMakeLists.txt` (top-level)
- Modify: `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/CMakeLists.txt` (drop sample-side linker hints; pass `USER_MODE_MULTITHREAD`)
- Modify: `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/wamr_lib.c` (one TU, two function bodies + dispatcher)

**Interfaces:**
- Consumes: `WAMR_BUILD_ZEPHYR_USERMODE_MT` from platform; `os_thread_create`, `os_thread_join`; `bh_queue_*`
- Produces: a sample that can be built in any of three combinations: (flag=1, MT=1), (flag=1, MT=0), (flag=0, MT=0). The fourth combo (flag=0, MT=1) is rejected at CMake time.

- [ ] **Step 1: Top-level CMakeLists — add `USER_MODE_MULTITHREAD` option and consistency check**

Edit `product-mini/platforms/zephyr/user-mode/CMakeLists.txt`. After the existing `option(WAMR_USE_PREBUILT_LIB ...)` line (~line 19), add:

```cmake
option(USER_MODE_MULTITHREAD
       "Build the multi-thread variant of the user-mode sample" ON)

if (USER_MODE_MULTITHREAD AND NOT WAMR_BUILD_ZEPHYR_USERMODE_MT EQUAL 1)
  message(FATAL_ERROR
    "USER_MODE_MULTITHREAD=ON requires -DWAMR_BUILD_ZEPHYR_USERMODE_MT=1. "
    "Either pass that flag, or build with -DUSER_MODE_MULTITHREAD=OFF for "
    "the single-thread variant (which still requires the flag to use "
    "bh_queue's condvar from user mode).")
endif ()

if (USER_MODE_MULTITHREAD)
  add_definitions(-DUSER_MODE_MULTITHREAD=1)
endif ()
```

Also set the flag's default in this sample to ON when not overridden — add near the top, after `project(...)`:

```cmake
if (NOT DEFINED WAMR_BUILD_ZEPHYR_USERMODE_MT)
  set (WAMR_BUILD_ZEPHYR_USERMODE_MT 1 CACHE STRING "")
endif ()
```

- [ ] **Step 2: Drop the sample-side linker hints from the library CMakeLists**

Edit `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/CMakeLists.txt`. Delete the entire `if (CONFIG_USERSPACE AND CONFIG_DYNAMIC_OBJECTS)` block (lines 87–109 pre-edit) — those linker hints now live in `shared_platform.cmake` (Task 6).

Also delete the auto-set of `WAMR_BUILD_THREAD_MGR` in `lib-wamr-zephyr/CMakeLists.txt` only if the top-level CMakeLists no longer needs it — leave it for now to be safe.

- [ ] **Step 3: Restructure `wamr_lib.c` to one TU + two function bodies + dispatcher**

Edit `product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/wamr_lib.c`. Add the ST function body before the existing `iwasm_main` body. New file structure:

```c
/* license header */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "bh_platform.h"
#include "bh_queue.h"
#include "wasm_export.h"
#include "test_wasm.h"   /* or riscv64 variant — keep existing #if block */

/* constants and globals (global_heap_buf, struct test_msg, struct worker_ctx
 * stay where they are) */

/* (MT case: keep existing worker_entry, keep existing iwasm_main but
 * rename to iwasm_main_mt) */

static void
iwasm_main_st(void)
{
    /* Single-thread variant: init runtime, instantiate one module,
     * run main, then one bh_queue round trip to exercise the
     * condvar code path under user mode. */
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    wasm_module_inst_t inst = NULL;
    bh_queue *queue = NULL;
    char error_buf[128];
    struct test_msg *msg = NULL;
    bh_message_t bmsg;

    printk("=== WAMR User-Mode ST + bh_queue Demo ===\n");

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

    if (!wasm_runtime_full_init(&init_args)) {
        printk("WAMR init failed\n");
        return;
    }

    module = wasm_runtime_load((uint8_t *)wasm_test_file,
                               sizeof(wasm_test_file), error_buf,
                               sizeof(error_buf));
    if (!module) {
        printk("load failed: %s\n", error_buf);
        goto cleanup;
    }

    inst = wasm_runtime_instantiate(module, APP_STACK_SIZE, APP_HEAP_SIZE,
                                    error_buf, sizeof(error_buf));
    if (!inst) {
        printk("instantiate failed: %s\n", error_buf);
        goto cleanup;
    }

    wasm_application_execute_main(inst, 0, NULL);

    queue = bh_queue_create();
    if (!queue) {
        printk("queue create failed\n");
        goto cleanup;
    }
    printk("bh_queue created (user mode)\n");

    msg = wasm_runtime_malloc(sizeof(struct test_msg));
    if (!msg) goto cleanup;
    msg->worker_id = 0;
    msg->seq = 0;
    snprintf(msg->payload, sizeof(msg->payload), "st-msg");
    if (!bh_post_msg(queue, 0, msg, sizeof(struct test_msg))) {
        wasm_runtime_free(msg);
        msg = NULL;
        printk("post failed\n");
        goto cleanup;
    }
    /* bh_post_msg takes ownership on success */
    msg = NULL;

    /* This is where flag=0 faults: bh_get_msg's condvar wait calls
     * k_condvar_wait, which validates the (unregistered, in WAMR heap)
     * condvar against the kernel object table and fails. With flag=1,
     * the condvar was allocated via k_object_alloc and validation
     * succeeds. */
    bmsg = bh_get_msg(queue, BHT_WAIT_FOREVER);
    if (bmsg) {
        struct test_msg *got = (struct test_msg *)bh_message_payload(bmsg);
        printk("  [recv] worker %d seq %d \"%s\"\n",
               got->worker_id, got->seq, got->payload);
        bh_free_msg(bmsg);
    }

cleanup:
    if (msg)
        wasm_runtime_free(msg);
    if (queue)
        bh_queue_destroy(queue);
    if (inst)
        wasm_runtime_deinstantiate(inst);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
    printk("=== ST Demo complete ===\n");
}

/* Multi-thread variant: existing iwasm_main body, renamed. */
static void
iwasm_main_mt(int num_workers)
{
    /* ... existing body of iwasm_main, replacing
     *     int num_workers = (int)(intptr_t)arg1;
     * with the function parameter. */
}

/* Dispatcher — what kernel main() spawns. */
void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    (void)arg2;
    (void)arg3;
#ifdef USER_MODE_MULTITHREAD
    iwasm_main_mt((int)(intptr_t)arg1);
#else
    (void)arg1;
    iwasm_main_st();
#endif
}
```

Verify with `grep -n 'iwasm_main' product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/wamr_lib.c` that there's exactly one public symbol named `iwasm_main`, plus the two static helpers.

- [ ] **Step 4: Build flag=1, MT=1 (default)**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -40
```

Expected: existing multi-thread demo output (worker count, message sequence, "=== Demo complete ===").

- [ ] **Step 5: Build flag=1, MT=0**

```bash
west build -b qemu_x86 . -p always -- -DUSER_MODE_MULTITHREAD=OFF
west build -t run 2>&1 | head -30
```

Expected:

```
=== WAMR User-Mode ST + bh_queue Demo ===
Hello world!
buf ptr: 0x1458
buf: 1234
bh_queue created (user mode)
  [recv] worker 0 seq 0 "st-msg"
=== ST Demo complete ===
```

- [ ] **Step 6: Build flag=0, MT=1 — must fail at CMake**

```bash
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0 2>&1 | tail -10
```

Expected: CMake error `USER_MODE_MULTITHREAD=ON requires -DWAMR_BUILD_ZEPHYR_USERMODE_MT=1`.

- [ ] **Step 7: Build flag=0, MT=0 — builds, runs, faults at first condvar use**

```bash
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0 -DUSER_MODE_MULTITHREAD=OFF
west build -t run 2>&1 | head -30
```

Expected: ST demo starts, prints `bh_queue created`, then board faults at the condvar wait (output along the lines of "address is not a known kernel object" or "Page fault"). Capture the exact output verbatim into the README in the next task.

- [ ] **Step 8: Commit**

```bash
git add product-mini/platforms/zephyr/user-mode/CMakeLists.txt \
        product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/CMakeLists.txt \
        product-mini/platforms/zephyr/user-mode/lib-wamr-zephyr/wamr_lib.c
git commit -m "$(cat <<'EOF'
sample/zephyr/user-mode: add USER_MODE_MULTITHREAD toggle

CMake-level:
  - New USER_MODE_MULTITHREAD option, default ON.
  - Consistency check: ON requires WAMR_BUILD_ZEPHYR_USERMODE_MT=1.
  - Sample defaults WAMR_BUILD_ZEPHYR_USERMODE_MT to 1.
  - Drop sample-side linker --undefined hints (moved platform-side).

Source-level: wamr_lib.c carries two static bodies, iwasm_main_mt and
iwasm_main_st, plus a dispatcher that picks between them based on
#ifdef USER_MODE_MULTITHREAD. The MT body is the existing worker-pool
demo unchanged. The ST body runs one WASM instance then performs one
bh_queue round trip (post + get_msg), which exercises the user-mode
condvar path with the simplest possible workload.

This makes the sample self-demonstrating: it covers three of the four
flag/MT combinations directly (the fourth is rejected at CMake time).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Create the `user-mode-app/` sample (Zephyr-app shape, user-mode ST + bh_queue)

**Files:**
- Create: `product-mini/platforms/zephyr/user-mode-app/CMakeLists.txt`
- Create: `product-mini/platforms/zephyr/user-mode-app/prj.conf`
- Create: `product-mini/platforms/zephyr/user-mode-app/src/main.c`
- Create: `product-mini/platforms/zephyr/user-mode-app/README.md`

**Interfaces:**
- Consumes: WAMR core sources (compiled directly into the Zephyr app); platform with flag enabled
- Produces: a sample that proves the flag is required even in Zephyr-app shape for user-mode + bh_queue

- [ ] **Step 1: Create `user-mode-app/CMakeLists.txt`**

```cmake
# Copyright (C) 2026 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wamr_user_mode_app LANGUAGES C)

enable_language(ASM)

set (WAMR_BUILD_PLATFORM "zephyr")

if (NOT DEFINED WAMR_BUILD_TARGET)
  set (WAMR_BUILD_TARGET "X86_32")
endif ()

if (NOT DEFINED WAMR_BUILD_INTERP)
  set (WAMR_BUILD_INTERP 1)
endif ()
if (NOT DEFINED WAMR_BUILD_AOT)
  set (WAMR_BUILD_AOT 1)
endif ()
if (NOT DEFINED WAMR_BUILD_LIBC_BUILTIN)
  set (WAMR_BUILD_LIBC_BUILTIN 1)
endif ()
if (NOT DEFINED WAMR_BUILD_LIBC_WASI)
  set (WAMR_BUILD_LIBC_WASI 0)
endif ()

if (NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_POOL)
  set (WAMR_BUILD_GLOBAL_HEAP_POOL 1)
endif ()
if (NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_SIZE)
  set (WAMR_BUILD_GLOBAL_HEAP_SIZE 65536)
endif ()

# Flag-toggle demo: default ON, override with -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0
# to demonstrate the failure mode.
if (NOT DEFINED WAMR_BUILD_ZEPHYR_USERMODE_MT)
  set (WAMR_BUILD_ZEPHYR_USERMODE_MT 1)
endif ()

set (WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

include (${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

target_sources(app PRIVATE
  ${WAMR_RUNTIME_LIB_SOURCE}
  src/main.c
)

target_compile_options(app PRIVATE -Wno-type-limits)
```

- [ ] **Step 2: Create `user-mode-app/prj.conf`**

```conf
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_USERSPACE=y
CONFIG_DYNAMIC_OBJECTS=y
CONFIG_HEAP_MEM_POOL_SIZE=4096
CONFIG_STACK_SENTINEL=y
CONFIG_PRINTK=y
CONFIG_LOG=y
```

- [ ] **Step 3: Create `user-mode-app/src/main.c`**

```c
/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <stdlib.h>
#include <string.h>
#include "bh_platform.h"
#include "bh_queue.h"
#include "wasm_export.h"
#include "test_wasm.h"

#define USER_THREAD_STACK 8192
#define APP_STACK_SIZE 8192
#define APP_HEAP_SIZE 8192
#define THREAD_PRIORITY 5

K_APPMEM_PARTITION_DEFINE(wamr_partition);
extern struct k_mem_partition z_libc_partition;

static struct k_mem_domain wamr_domain;

K_APP_DMEM(wamr_partition)
static char global_heap_buf[WASM_GLOBAL_HEAP_SIZE] = { 0 };

K_THREAD_STACK_DEFINE(user_thread_stack, USER_THREAD_STACK);
static struct k_thread user_thread;

#if defined(CONFIG_USERSPACE)
extern void
os_thread_env_init_for_usermode(k_tid_t tid);
#endif

struct test_msg {
    int worker_id;
    int seq;
    char payload[32];
};

static void
user_entry(void *a1, void *a2, void *a3)
{
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    wasm_module_inst_t inst = NULL;
    bh_queue *queue = NULL;
    char error_buf[128];
    struct test_msg *msg;
    bh_message_t bmsg;

    (void)a1; (void)a2; (void)a3;

    printk("=== user-mode-app ST demo (Zephyr-app shape) ===\n");

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
    if (!wasm_runtime_full_init(&init_args)) {
        printk("WAMR init failed\n");
        return;
    }

    module = wasm_runtime_load((uint8_t *)wasm_test_file,
                               sizeof(wasm_test_file), error_buf,
                               sizeof(error_buf));
    if (!module) { printk("load: %s\n", error_buf); goto cleanup; }

    inst = wasm_runtime_instantiate(module, APP_STACK_SIZE, APP_HEAP_SIZE,
                                    error_buf, sizeof(error_buf));
    if (!inst) { printk("instantiate: %s\n", error_buf); goto cleanup; }

    wasm_application_execute_main(inst, 0, NULL);

    queue = bh_queue_create();
    if (!queue) { printk("queue create failed\n"); goto cleanup; }
    printk("bh_queue created (user mode)\n");

    msg = wasm_runtime_malloc(sizeof(struct test_msg));
    if (!msg) goto cleanup;
    msg->worker_id = 0;
    msg->seq = 0;
    snprintf(msg->payload, sizeof(msg->payload), "app-st-msg");
    if (!bh_post_msg(queue, 0, msg, sizeof(struct test_msg))) {
        wasm_runtime_free(msg);
        goto cleanup;
    }

    /* Faults here when WAMR_BUILD_ZEPHYR_USERMODE_MT=0 — the condvar
     * inside bh_queue was BH_MALLOC'd, not k_object_alloc'd, so syscall
     * validation rejects it. With the flag on, condvar is registered. */
    bmsg = bh_get_msg(queue, BHT_WAIT_FOREVER);
    if (bmsg) {
        struct test_msg *got = (struct test_msg *)bh_message_payload(bmsg);
        printk("  [recv] worker %d seq %d \"%s\"\n",
               got->worker_id, got->seq, got->payload);
        bh_free_msg(bmsg);
    }

cleanup:
    if (queue) bh_queue_destroy(queue);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    wasm_runtime_destroy();
    printk("=== Demo complete ===\n");
}

int
main(void)
{
    struct k_mem_partition *parts[] = { &wamr_partition, &z_libc_partition };

    if (k_mem_domain_init(&wamr_domain, 2, parts) != 0) {
        printk("Failed to init memory domain\n");
        return -1;
    }

    k_tid_t tid = k_thread_create(
        &user_thread, user_thread_stack, USER_THREAD_STACK,
        user_entry, NULL, NULL, NULL,
        THREAD_PRIORITY, K_USER, K_FOREVER);

    k_mem_domain_add_thread(&wamr_domain, tid);

#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT
    os_thread_env_init_for_usermode(tid);
#endif

#if KERNEL_VERSION_NUMBER >= 0x040000
    k_wakeup(tid);
#else
    k_thread_start(tid);
#endif

    k_thread_join(&user_thread, K_FOREVER);
    return 0;
}
```

- [ ] **Step 4: Build flag=1 (default), run**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode-app
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -30
```

Expected:

```
=== user-mode-app ST demo (Zephyr-app shape) ===
Hello world!
buf ptr: 0x1458
buf: 1234
bh_queue created (user mode)
  [recv] worker 0 seq 0 "app-st-msg"
=== Demo complete ===
```

- [ ] **Step 5: Build flag=0, run — must fault**

```bash
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0
west build -t run 2>&1 | head -30
```

Expected: demo starts, prints `bh_queue created`, then board faults. Capture the exact fault output for the README.

- [ ] **Step 6: Create `user-mode-app/README.md`**

(See Task 11 for the README template. For this task, write a minimal placeholder README so the directory has a top-line description; the full README content is written in Task 11.)

Minimal placeholder:

```markdown
# WAMR Zephyr User-Mode App — Single-Thread + bh_queue Demo

Zephyr-app shape (WAMR sources compiled directly into the app target).
Demonstrates that `WAMR_BUILD_ZEPHYR_USERMODE_MT` is required even for
single-thread user-mode if the workload uses `bh_queue`'s condvar.

See `README.md` for build instructions (filled in by Task 11).
```

- [ ] **Step 7: Commit**

```bash
git add product-mini/platforms/zephyr/user-mode-app/
git commit -m "$(cat <<'EOF'
sample/zephyr/user-mode-app: new Zephyr-app shape sample

Mirrors simple/'s layout (target_sources directly on the app target,
no zephyr_library) but adds CONFIG_USERSPACE + CONFIG_DYNAMIC_OBJECTS
and spawns one user-mode thread that runs WASM then does a bh_queue
round trip.

Flag default is ON. Build with -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0 to
reproduce the failure mode: the demo starts, bh_queue is created, then
the first bh_get_msg's condvar wait faults because the condvar wasn't
registered via k_object_alloc.

Full README with both flag invocations and expected outputs lands
in a follow-up commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Consolidate user-mode docs into `docs/zephyr-usermode-internals.md`

**Files:**
- Create: `docs/zephyr-usermode-internals.md`
- Delete: `product-mini/platforms/zephyr/user-mode/USERMODE.md` (after content move)
- Delete: `product-mini/platforms/zephyr/user-mode/USERMODE_MULTITHREAD.md` (after content move)

**Interfaces:** documentation only — no code interfaces.

- [ ] **Step 1: Read both source files in full**

```bash
cat /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode/USERMODE.md
cat /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode/USERMODE_MULTITHREAD.md
```

(Already cached earlier in this conversation; refer back to the explanation of the kobject registration gap, K_INHERIT_PERMS, linker --undefined, and the join race.)

- [ ] **Step 2: Write the consolidated doc**

Create `docs/zephyr-usermode-internals.md`. Structure:

```markdown
# Zephyr User-Mode Multi-Thread: Platform Internals

Reference for understanding why `WAMR_BUILD_ZEPHYR_USERMODE_MT` exists,
what it changes, and which Zephyr Kconfigs interact with it.

## The kernel-object registration gap

(Pull the diagram + explanation from USERMODE_MULTITHREAD.md "What We
Solved" + "The Root Cause" sections.)

## Fix 1: Dynamic kobject allocation

(USERMODE_MULTITHREAD.md "Fix 1" — the zmutex_t / zsem_t pointer trick.)

## Fix 2: Permission inheritance via K_INHERIT_PERMS

(USERMODE_MULTITHREAD.md "Fix 2" plus the MPU stack chicken-and-egg
note.)

## Fix 3: Linker `--undefined` hints for k_condvar

(USERMODE_MULTITHREAD.md "Fix 3" — explain Zephyr's two-section link
and why condvar symbols need pre-marking.)

## Fix 4: Thread join race

(USERMODE_MULTITHREAD.md "Fix 4" — what the race was and the
exited-flag fix. Note this fix is unconditional in WAMR, benefits
kernel-mode MT too.)

## When the flag is needed

The flag is needed when all of:
- `CONFIG_USERSPACE=y` (Zephyr enforces syscall-based kobject validation)
- WAMR's threading API in use (`os_thread_create`, `bh_queue`, etc.)
- One of: multiple user-mode threads, OR a user-mode thread that uses
  bh_queue's condvar (any contention path that takes a slow syscall)

It's NOT needed when:
- Kernel-mode only (any threading model)
- User-mode single-thread that touches *only* mutexes and not condvars
  (Zephyr's sys_mutex fast path skips the syscall)

## What moving from `wamr_lib` to direct app layout (or vice versa) changes

(Lift the Zephyr-app vs. Zephyr-library comparison table from
USERMODE.md, especially the "What Cannot Move" section.)

## Configuration cheat sheet

```conf
# prj.conf for user-mode multi-thread
CONFIG_USERSPACE=y
CONFIG_DYNAMIC_OBJECTS=y
CONFIG_HEAP_MEM_POOL_SIZE=4096   # k_object_alloc draws from this heap
CONFIG_DYNAMIC_THREAD=y          # only if you also k_thread_stack_alloc
```

```cmake
# CMake — sample-side
set(WAMR_BUILD_ZEPHYR_USERMODE_MT 1)
```
```

Fill in each section by copying the corresponding paragraphs from the
two source files. Edit for cohesion (single voice, no duplicated
prose) but keep technical claims verbatim.

- [ ] **Step 3: Delete the two old files**

```bash
git rm product-mini/platforms/zephyr/user-mode/USERMODE.md
git rm product-mini/platforms/zephyr/user-mode/USERMODE_MULTITHREAD.md
```

Note: these files are currently *untracked* (per `git status` they show as `??`), so `git rm` will fail. Use plain `rm` instead:

```bash
rm product-mini/platforms/zephyr/user-mode/USERMODE.md
rm product-mini/platforms/zephyr/user-mode/USERMODE_MULTITHREAD.md
```

- [ ] **Step 4: Commit**

`docs/` is in local `.gitignore`, so use `-f`:

```bash
git add -f docs/zephyr-usermode-internals.md
git commit -m "$(cat <<'EOF'
docs(zephyr): consolidate user-mode internals reference

Merge the persistent content from
product-mini/platforms/zephyr/user-mode/USERMODE.md and
USERMODE_MULTITHREAD.md into one platform-internals doc:
docs/zephyr-usermode-internals.md.

The doc covers the kobject registration gap, the four fixes
(dynamic alloc, K_INHERIT_PERMS, linker --undefined, join race),
when the flag is needed, and a configuration cheat sheet.

The two source files are deleted (they were untracked
working-tree-only notes anyway).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: Update per-sample READMEs

**Files:**
- Modify: `product-mini/platforms/zephyr/user-mode/README.md`
- Modify: `product-mini/platforms/zephyr/user-mode-app/README.md` (replace placeholder from Task 9)

**Interfaces:** documentation only.

- [ ] **Step 1: Update `user-mode/README.md`**

Find the section that describes the multi-thread demo (line ~135 onward, per the branch's diff) and ensure it lists the three supported build combinations: (flag=1, MT=1), (flag=1, MT=0), (flag=0, MT=0), with the expected output of each (or "faults at first condvar wait" for the last). Reference `docs/zephyr-usermode-internals.md` for the "why".

Add a short table near the top of the file:

```markdown
## Build matrix

| `WAMR_BUILD_ZEPHYR_USERMODE_MT` | `USER_MODE_MULTITHREAD` | Outcome |
|---|---|---|
| 1 (default) | ON (default) | Worker-pool + bh_queue demo |
| 1 | OFF | Single user-mode thread + bh_queue round trip |
| 0 | OFF | ST demo starts, faults at first condvar wait |
| 0 | ON | Rejected by CMake |

See `docs/zephyr-usermode-internals.md` for why the flag is needed.
```

- [ ] **Step 2: Replace `user-mode-app/README.md` placeholder with the full README**

```markdown
# WAMR Zephyr User-Mode App — Single-Thread + bh_queue Demo

This sample runs WAMR in user mode in **Zephyr-app shape** (WAMR sources
compiled directly into the app target, like `simple/`), spawning one
user-mode thread that runs a WASM app and then performs a single
`bh_queue` round trip. Its job is to demonstrate that
`WAMR_BUILD_ZEPHYR_USERMODE_MT` is required even for single-thread
user-mode work *if* the workload touches `bh_queue`'s condition variable.

See `docs/zephyr-usermode-internals.md` for the platform-layer
background.

## Build matrix

| `WAMR_BUILD_ZEPHYR_USERMODE_MT` | Outcome |
|---|---|
| 1 (default) | Demo completes |
| 0 | Demo starts, faults at first `bh_get_msg` (condvar wait) |

## Run with flag on (default)

```shell
west build -b qemu_x86 . -p always
west build -t run
```

Expected output:

```
*** Booting Zephyr OS build ... ***
=== user-mode-app ST demo (Zephyr-app shape) ===
Hello world!
buf ptr: 0x1458
buf: 1234
bh_queue created (user mode)
  [recv] worker 0 seq 0 "app-st-msg"
=== Demo complete ===
```

## Run with flag off — demonstrates the failure mode

```shell
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0
west build -t run
```

Expected output:

```
*** Booting Zephyr OS build ... ***
=== user-mode-app ST demo (Zephyr-app shape) ===
Hello world!
buf ptr: 0x1458
buf: 1234
bh_queue created (user mode)
<err> os: <0x...> is not a valid object
<err> os: ZEPHYR FATAL ERROR ...
```

(The exact fault message depends on Zephyr version; the key signal is
that the board faults at the first `bh_get_msg` rather than completing
the demo. Replace the snippet above with the verbatim output you
captured in Task 9 Step 5.)

## Why the flag is needed

`bh_queue` allocates its internal mutex and condvar via `BH_MALLOC`
into the WAMR heap. Under `CONFIG_USERSPACE`:

- Mutexes have a fast path (atomic-CAS) that skips the syscall when
  there's no contention, so single-thread mutex work "accidentally
  works" without registration.
- Condvars have no such fast path — every `bh_get_msg` call enters the
  kernel via `k_condvar_wait`, which validates the condvar against the
  kernel object table. An unregistered condvar fails the check.

`WAMR_BUILD_ZEPHYR_USERMODE_MT=1` switches `bh_queue`'s condvar (and
WAMR's other kobjects) to use `k_object_alloc`, which registers them
in the table.
```

- [ ] **Step 3: Commit**

```bash
git add product-mini/platforms/zephyr/user-mode/README.md \
        product-mini/platforms/zephyr/user-mode-app/README.md
git commit -m "$(cat <<'EOF'
docs(zephyr): per-sample READMEs for ST/MT and flag-toggle demos

user-mode/README.md: document the four-cell build matrix
(flag * USER_MODE_MULTITHREAD) with expected behavior of each cell.

user-mode-app/README.md: full content (replacing Task 9 placeholder)
explaining the flag-on / flag-off contrast with both expected outputs
captured verbatim from the qemu_x86 runs.

Both READMEs reference docs/zephyr-usermode-internals.md for the
platform-internals "why".

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Re-verify all four/five build configurations end-to-end

**Files:** none modified — verification only.

**Interfaces:** confirms the spec's acceptance criteria 3–6 pass.

- [ ] **Step 1: simple/ (flag=0, kernel MT)**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/simple
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -30
```

Expected: kernel-MT demo from Task 7, 4 messages received, no faults.

- [ ] **Step 2: user-mode/ (flag=1, MT=1) — current branch behavior**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -40
```

Expected: existing worker-pool + bh_queue demo from the current branch tip.

- [ ] **Step 3: user-mode/ (flag=1, MT=0)**

```bash
west build -b qemu_x86 . -p always -- -DUSER_MODE_MULTITHREAD=OFF
west build -t run 2>&1 | head -30
```

Expected: ST demo, 1 message received, no faults.

- [ ] **Step 4: user-mode/ (flag=0, MT=1) — rejected at CMake**

```bash
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0 2>&1 | tail -10
```

Expected: `FATAL_ERROR: USER_MODE_MULTITHREAD=ON requires ...`

- [ ] **Step 5: user-mode-app/ (flag=1)**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode-app
west build -b qemu_x86 . -p always
west build -t run 2>&1 | head -30
```

Expected: ST demo from Task 9 Step 4.

- [ ] **Step 6: user-mode-app/ (flag=0) — demonstrates fault**

```bash
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0
west build -t run 2>&1 | head -30
```

Expected: fault output matching what's documented in `user-mode-app/README.md`.

- [ ] **Step 7: If qemu_arc available, repeat steps 2 and 5 on qemu_arc/qemu_arc_hs**

```bash
cd /home/tl/projects/wasm-micro-runtime/product-mini/platforms/zephyr/user-mode
west build -b qemu_arc/qemu_arc_hs . -p always -- -DWAMR_BUILD_TARGET=ARC
west build -t run 2>&1 | head -40
```

(Optional — qemu_x86 verification is sufficient for the acceptance gate.)

- [ ] **Step 8: No commit — verification step only**

If any step deviates from expected output, return to the relevant earlier task to fix.

---

### Task 13: Final cleanup — search for stale references

**Files:** modifications only if needed.

- [ ] **Step 1: Search for any remaining `CONFIG_USERSPACE && CONFIG_DYNAMIC_OBJECTS` references**

```bash
grep -rn 'CONFIG_USERSPACE.*CONFIG_DYNAMIC_OBJECTS' core/ product-mini/platforms/zephyr/ 2>/dev/null
```

Expected: no matches.

- [ ] **Step 2: Search for stale references to `os_thread_obj`, `dyn_thread_node`**

```bash
grep -rn 'os_thread_obj\|dyn_thread_node\|dyn_thread_list' core/ 2>/dev/null
```

Expected: no matches (these types were internal to `zephyr_thread.c`).

- [ ] **Step 3: Search for stale references to `USERMODE.md` / `USERMODE_MULTITHREAD.md`**

```bash
grep -rn 'USERMODE\.md\|USERMODE_MULTITHREAD\.md' --include='*.md' --include='*.txt' --include='*.cmake' product-mini/ docs/ 2>/dev/null
```

Expected: no matches (or only matches inside the consolidated `docs/zephyr-usermode-internals.md` if it intentionally refers to them by old name).

- [ ] **Step 4: If any matches found in Steps 1–3, fix them and commit**

Otherwise no commit needed.

- [ ] **Step 5: Confirm spec acceptance criteria one-by-one**

Open `docs/superpowers/specs/2026-06-26-zephyr-usermode-separation-design.md`,
read the "Acceptance criteria" section, and check each item against the
implemented state. If any criterion is unmet, return to the relevant
task. If all pass, the implementation is complete.

---

## Plan Self-Review

**Spec coverage check:**
- ✅ Flag (`WAMR_BUILD_ZEPHYR_USERMODE_MT`) → Task 1
- ✅ Gate replacement in `platform_internal.h` → Task 5
- ✅ Gate replacement in `zephyr_thread.c` → Task 4
- ✅ Unified `thread_obj_node` list → Task 4 Steps 3–6
- ✅ `zephyr_thread_usermode.c` split → Tasks 2 + 3
- ✅ Join-race fix kept unconditional → Task 4 (existing code preserved, no #ifdef added around it)
- ✅ Platform-side linker hints → Task 6
- ✅ `simple/` MT upgrade → Task 7
- ✅ `user-mode/` ST/MT toggle → Task 8
- ✅ `user-mode-app/` new sample → Task 9
- ✅ Doc consolidation → Task 10
- ✅ Per-sample READMEs → Task 11
- ✅ Verification of all build cells → Task 12

**Placeholder scan:** No TBDs, no "implement later", no untyped references. Task 3 Step 1 has a conditional ("if the extern doesn't compile, switch to helper pattern") with both branches fully specified — that's a decision point, not a placeholder.

**Type consistency:** `thread_obj_node` (Task 4 Step 3) is referenced consistently in Steps 4–6. `dyn_thread_alloc` / `dyn_thread_release` signatures match across Tasks 2 (header) and 3 (impl). `iwasm_main_st` / `iwasm_main_mt` (Task 8 Step 3) — both internal helpers, dispatched from `iwasm_main`.

**Risk callouts:**
- Task 3 Step 2's decision (extern vs. helper pattern for mpu_stacks) is conditional on a Zephyr macro detail. If you can't build Zephyr, pick the helper pattern — it's more portable and Task 4 Step 7 already accounts for it.
- Task 8 Step 6/7 and Task 9 Step 5 (board faults) are intentional. Capture the exact fault output rather than skipping — the READMEs reference it verbatim.
- Tasks 4 and 7 both touch the same logical area (thread plumbing + simple/ sample). Task 7 depends on Task 4 having landed.

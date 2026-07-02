/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/app_memory/app_memdomain.h>

#define MAIN_THREAD_STACK_SIZE 8192
#define THREAD_PRIORITY 5
#define NUM_WORKERS 2

K_APPMEM_PARTITION_DEFINE(wamr_partition);
extern struct k_mem_partition z_libc_partition;

static struct k_mem_domain wamr_domain;

K_THREAD_STACK_DEFINE(wamr_main_stack, MAIN_THREAD_STACK_SIZE);
static struct k_thread wamr_main_thread;

/* Entry point defined in wamr_lib.c — takes num_workers as arg1 */
extern void
iwasm_main(void *arg1, void *arg2, void *arg3);

/* Prepare a user-mode thread to call os_thread_create().
 * Grants access to WAMR's internal MPU-aligned thread stacks — required
 * because k_thread_create validates that the *calling* thread has
 * permission to the stack object being passed. Must be called from
 * kernel mode before the thread starts.
 * Defined in core/shared/platform/zephyr/zephyr_thread_usermode.c,
 * only compiled when WAMR_BUILD_ZEPHYR_USERMODE_MT=1.
 *
 * Gating note: WAMR_BUILD_ZEPHYR_USERMODE_MT is added to the WAMR-library
 * scope by config_common.cmake but is NOT propagated to the app target
 * compiling this file. USER_MODE_MULTITHREAD is the app-scope macro and
 * the CMake consistency check guarantees USER_MODE_MULTITHREAD=ON implies
 * the platform flag is on, so the two are equivalent here. */
#ifdef USER_MODE_MULTITHREAD
extern void
os_thread_env_init_for_usermode(k_tid_t tid);
#endif

int
main(void)
{
    /* Set up memory domain with WAMR partition and libc partition */
    struct k_mem_partition *parts[] = { &wamr_partition, &z_libc_partition };
    if (k_mem_domain_init(&wamr_domain, 2, parts) != 0) {
        printk("Failed to init memory domain\n");
        return -1;
    }

    /* Spawn user-mode WAMR main thread.
     * iwasm_main (in wamr_lib.c) inits WAMR, creates bh_queue, spawns
     * workers via os_thread_create with K_INHERIT_PERMS. */
    k_tid_t tid = k_thread_create(
        &wamr_main_thread, wamr_main_stack, MAIN_THREAD_STACK_SIZE,
        iwasm_main, (void *)(intptr_t)NUM_WORKERS, NULL, NULL,
        THREAD_PRIORITY, K_USER, K_FOREVER);

    k_mem_domain_add_thread(&wamr_domain, tid);

    /* Probe 1: grant the user-mode thread access to the K_SEM_DEFINE'd
     * semaphore declared inside wamr_lib.c (partitioned TU). If the
     * grant succeeds AND the user-mode thread can subsequently
     * k_sem_give/k_sem_take, that proves K_SEM_DEFINE inside a
     * zephyr_library_app_memory partition IS gperf-visible and works
     * via the standard grant pattern. */
    extern struct k_sem wamr_partition_sem_probe;
    k_object_access_grant(&wamr_partition_sem_probe, tid);

    /* Probe 2: try to k_wakeup a K_THREAD_DEFINE'd thread declared
     * inside wamr_lib.c (partitioned TU) — from SUPERVISOR MODE (i.e.
     * right here in main(), which runs kernel-mode). Under
     * CONFIG_USERSPACE the syscall validator normally runs only for
     * user-mode callers, but kobject registration is still what
     * gperf produces; if the k_thread isn't there, k_object_find()
     * returns NULL and the k_wakeup either faults or silently no-ops
     * depending on the code path. Empirically the failure is quiet
     * from supervisor (kernel bypasses validation) but the same call
     * from user mode faults with "not a valid k_thread". See
     * docs/zephyr-usermode-internals.md, "The Kernel-Object
     * Registration Gap" for the mechanism.
     *
     * This probe demonstrates the second gperf hole: K_THREAD_DEFINE
     * uses the _static_thread_data iterable section, which the
     * scanner filters by section name — and zephyr_library_app_memory
     * remaps that section into wamr_partition, hiding it. */
    extern const k_tid_t wamr_partition_kthread_probe;
    printk("[probe2] wamr_partition_kthread_probe tid = %p\n",
           wamr_partition_kthread_probe);
    /* Not k_wakeup'd — see comment; if we did, from supervisor it would
     * still work despite the missing gperf entry (kernel path uses the
     * pointer directly), but the thread would MPU-fault on any
     * partition access anyway. */

#ifdef USER_MODE_MULTITHREAD
    os_thread_env_init_for_usermode(tid);
#endif

#if KERNEL_VERSION_NUMBER >= 0x040000
    k_wakeup(tid);
#else
    k_thread_start(tid);
#endif

    k_thread_join(&wamr_main_thread, K_FOREVER);
    return 0;
}

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
 * only compiled when WAMR_BUILD_ZEPHYR_USERMODE_MT=1. */
#if WAMR_BUILD_ZEPHYR_USERMODE_MT
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

#if WAMR_BUILD_ZEPHYR_USERMODE_MT
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

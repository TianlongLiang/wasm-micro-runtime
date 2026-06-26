/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "zephyr_thread_internal.h"

#ifdef WAMR_BUILD_ZEPHYR_USERMODE_MT

/* Duplicated from zephyr_thread.c for visibility in this TU */
#if defined(CONFIG_ARM_MPU) || defined(CONFIG_ARC_MPU) \
    || KERNEL_VERSION_NUMBER > 0x020300 /* version 2.3.0 */
#define BH_ENABLE_ZEPHYR_MPU_STACK 1
#elif !defined(BH_ENABLE_ZEPHYR_MPU_STACK)
#define BH_ENABLE_ZEPHYR_MPU_STACK 0
#endif
#if !defined(BH_ZEPHYR_MPU_STACK_COUNT)
#define BH_ZEPHYR_MPU_STACK_COUNT 4
#endif

#if BH_ENABLE_ZEPHYR_MPU_STACK != 0
extern void *mpu_stack_addr(int i);
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
        void *p = mpu_stack_addr(i);
        if (p)
            k_object_access_grant(p, tid);
    }
#else
    (void)tid;
#endif
}

#endif /* WAMR_BUILD_ZEPHYR_USERMODE_MT */

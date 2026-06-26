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

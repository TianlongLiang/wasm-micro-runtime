/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef ZEPHYR_POOL_OWNER_COMPAT_H
#define ZEPHYR_POOL_OWNER_COMPAT_H

typedef enum {
    WAMR_ZEPHYR_POOL_OWNER_INVALID,
    WAMR_ZEPHYR_POOL_OWNER_ACCESSIBLE,
    WAMR_ZEPHYR_POOL_OWNER_NEEDS_ACCESS,
} wamr_zephyr_pool_owner_validation_t;

#if defined(CONFIG_USERSPACE)

#include <zephyr/kernel.h>
#include <zephyr/version.h>

#if KERNEL_VERSION_NUMBER < ZEPHYR_VERSION(3, 7, 0)
#error "Zephyr userspace pool owners require Zephyr 3.7.0 or newer"
#endif

/* Zephyr 3.7 and 4.4 k_object_is_valid() ask the kernel validator for
 * K_OBJ_ANY/_OBJ_INIT_ANY, so the public API cannot prove that an inaccessible
 * owner is an initialized thread before access is granted. The internal
 * lookup/validation behavior below was build/runtime verified on pinned 3.7.0
 * and forward-probed 4.4.0/4.4.1, but is not a stable public API. Keep the
 * dependency confined here so incompatible future versions fail compilation
 * and can be revalidated.
 */
#include <zephyr/internal/syscall_handler.h>

static inline wamr_zephyr_pool_owner_validation_t
wamr_zephyr_pool_owner_validate(k_tid_t owner)
{
    int validation_result = k_object_validate(
        k_object_find(owner), K_OBJ_THREAD, _OBJ_INIT_TRUE);

    if (validation_result == 0) {
        return WAMR_ZEPHYR_POOL_OWNER_ACCESSIBLE;
    }
    if (validation_result == -EPERM) {
        return WAMR_ZEPHYR_POOL_OWNER_NEEDS_ACCESS;
    }
    return WAMR_ZEPHYR_POOL_OWNER_INVALID;
}

#else

static inline wamr_zephyr_pool_owner_validation_t
wamr_zephyr_pool_owner_validate(k_tid_t owner)
{
    ARG_UNUSED(owner);
    return WAMR_ZEPHYR_POOL_OWNER_ACCESSIBLE;
}

#endif /* CONFIG_USERSPACE */

#endif /* ZEPHYR_POOL_OWNER_COMPAT_H */

/*
 * Copyright (C) 2026 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "test_common.h"

#include "platform_api_vmcore.h"

static uint8_t exec_buffer[128] __aligned(8);
static uint32_t alloc_size;
static void *freed_address;

static void *
test_exec_alloc(uint32_t size)
{
    alloc_size = size;
    return size <= sizeof(exec_buffer) ? exec_buffer : NULL;
}

static void
test_exec_free(void *address)
{
    freed_address = address;
}

static void
platform_general_before(void *fixture)
{
    pool_before(fixture);
}

static void
platform_general_after(void *fixture)
{
    set_exec_mem_alloc_func(NULL, NULL);
    pool_after(fixture);
}

ZTEST_SUITE(platform_general, NULL, NULL, platform_general_before,
            platform_general_after, NULL);

/* Catches a regression where os_realloc() does not preserve the old block. */
ZTEST(platform_general, test_heap_wrappers_preserve_reallocated_data)
{
    uint8_t *memory = os_malloc(64U);

    zassert_not_null(memory, "os_malloc failed");
    memset(memory, 0xA5, 64U);
    memory = os_realloc(memory, 128U);
    zassert_not_null(memory, "os_realloc failed");
    for (size_t i = 0; i < 64U; ++i) {
        zassert_equal(memory[i], 0xA5, "reallocation lost byte %zu", i);
    }
    os_free(memory);
    os_free(NULL);
}

/* Catches a regression where os_mmap() skips callback allocation or clearing.
 */
ZTEST(platform_general, test_mmap_uses_exec_callbacks_and_clears_memory)
{
    uint8_t *memory;

    alloc_size = 0U;
    freed_address = NULL;
    memset(exec_buffer, 0xA5, sizeof(exec_buffer));
    set_exec_mem_alloc_func(test_exec_alloc, test_exec_free);

    memory = os_mmap(NULL, 64U, 0, 0, os_get_invalid_handle());
    zassert_equal(memory, exec_buffer, "callback allocation was not used");
    zassert_equal(alloc_size, 64U, "callback received incorrect size");
    for (size_t i = 0; i < 64U; ++i) {
        zassert_equal(memory[i], 0U, "mapped byte %zu was not cleared", i);
    }

    os_munmap(memory, 64U);
    zassert_equal(freed_address, memory,
                  "callback free received incorrect address");
    set_exec_mem_alloc_func(NULL, NULL);
}

/* Catches a regression where os_mmap() reports callback allocation failure. */
ZTEST(platform_general, test_mmap_propagates_exec_callback_failure)
{
    alloc_size = 0U;
    set_exec_mem_alloc_func(test_exec_alloc, test_exec_free);

    zassert_is_null(os_mmap(NULL, 129U, 0, 0, os_get_invalid_handle()),
                    "oversized callback allocation succeeded");
    zassert_equal(alloc_size, 129U, "callback did not receive requested size");
    set_exec_mem_alloc_func(NULL, NULL);
}

/* Catches a regression where os_mmap() truncates UINT32_MAX before rejecting.
 */
ZTEST(platform_general, test_mmap_rejects_uint32_max_without_callback)
{
    alloc_size = 0U;
    set_exec_mem_alloc_func(test_exec_alloc, test_exec_free);

    zassert_is_null(os_mmap(NULL, UINT32_MAX, 0, 0, os_get_invalid_handle()),
                    "UINT32_MAX mapping was accepted");
    zassert_equal(alloc_size, 0U, "rejected mapping invoked callback");
    set_exec_mem_alloc_func(NULL, NULL);
}

/* Catches a regression where default os_mmap() does not return cleared memory.
 */
ZTEST(platform_general, test_default_mmap_returns_zeroed_memory)
{
    uint8_t *memory = os_mmap(NULL, 64U, 0, 0, os_get_invalid_handle());

    zassert_not_null(memory, "default os_mmap failed");
    for (size_t i = 0; i < 64U; ++i) {
        zassert_equal(memory[i], 0U, "mapped byte %zu was not cleared", i);
    }
    os_munmap(memory, 64U);
}

/* Catches a regression where os_mremap() drops the original mapping prefix. */
ZTEST(platform_general, test_mremap_preserves_existing_prefix)
{
    uint8_t *memory = os_mmap(NULL, 32U, 0, 0, os_get_invalid_handle());
    uint8_t *remapped;

    zassert_not_null(memory, "initial os_mmap failed");
    for (size_t i = 0; i < 32U; ++i) {
        memory[i] = (uint8_t)i;
    }

    remapped = os_mremap(memory, 32U, 64U);
    zassert_not_null(remapped, "os_mremap failed");
    for (size_t i = 0; i < 32U; ++i) {
        zassert_equal(remapped[i], (uint8_t)i, "remapping lost byte %zu", i);
    }
    os_munmap(remapped, 64U);
}

/* Catches a regression where Zephyr's no-op protection wrapper reports failure.
 */
ZTEST(platform_general, test_mprotect_accepts_valid_memory)
{
    zassert_equal(os_mprotect(exec_buffer, sizeof(exec_buffer), 0), 0,
                  "os_mprotect failed");
}

/* Catches regressions in Zephyr's documented sentinel and utility wrappers. */
ZTEST(platform_general, test_miscellaneous_platform_contracts)
{
    zassert_equal(os_dumps_proc_mem_info(NULL, 0U), -1,
                  "unsupported memory report changed contract");
    zassert_equal(os_invalid_raw_handle(), -1,
                  "invalid raw handle changed contract");
    zassert_is_null(os_get_invalid_handle(),
                    "invalid platform handle changed contract");
    zassert_true(os_getpagesize() > 0, "page size must be positive");
    zassert_equal(os_sched_yield(), 0, "yield wrapper failed");
    os_dcache_flush();
    os_icache_flush(exec_buffer, sizeof(exec_buffer));
}

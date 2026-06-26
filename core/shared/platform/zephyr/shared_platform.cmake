# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set (PLATFORM_SHARED_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions(-DBH_PLATFORM_ZEPHYR)

include_directories(${PLATFORM_SHARED_DIR})
include_directories(${PLATFORM_SHARED_DIR}/../include)

file (GLOB_RECURSE source_all ${PLATFORM_SHARED_DIR}/*.c)

if(${CONFIG_MINIMAL_LIBC})
    include (${CMAKE_CURRENT_LIST_DIR}/../common/math/platform_api_math.cmake)
    set (source_all ${source_all} ${PLATFORM_COMMON_MATH_SOURCE})
endif()

if (NOT WAMR_BUILD_LIBC_WASI EQUAL 1)
    list(REMOVE_ITEM source_all ${PLATFORM_SHARED_DIR}/zephyr_socket.c)
    list(REMOVE_ITEM source_all ${PLATFORM_SHARED_DIR}/zephyr_file.c)
    list(REMOVE_ITEM source_all ${PLATFORM_SHARED_DIR}/zephyr_clock.c)
else()
  include (${CMAKE_CURRENT_LIST_DIR}/../common/libc-util/platform_common_libc_util.cmake)
  set(source_all ${source_all} ${PLATFORM_COMMON_LIBC_UTIL_SOURCE})
endif ()

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

set (PLATFORM_SHARED_SOURCE ${source_all})

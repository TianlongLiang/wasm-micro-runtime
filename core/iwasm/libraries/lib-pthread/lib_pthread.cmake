# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set (LIB_PTHREAD_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions (-DWASM_ENABLE_LIB_PTHREAD=1)

if (WAMR_BUILD_LIB_PTHREAD_SEMAPHORE EQUAL 1)
    add_definitions (-DWASM_ENABLE_LIB_PTHREAD_SEMAPHORE=1)
endif()

include_directories(${LIB_PTHREAD_DIR})

file (GLOB source_all ${LIB_PTHREAD_DIR}/*.c)

set (LIB_PTHREAD_SOURCE ${source_all})
list (APPEND WAMR_NATIVE_API_SOURCES ${LIB_PTHREAD_DIR}/lib_pthread_wrapper.c)


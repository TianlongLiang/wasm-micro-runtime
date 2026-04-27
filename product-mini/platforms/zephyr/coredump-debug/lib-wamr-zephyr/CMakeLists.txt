# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)

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

# Enable call stack dump on WASM trap — prints function names + offsets
set (WAMR_BUILD_DUMP_CALL_STACK 1)

# Load function names from the custom name section
set (WAMR_BUILD_CUSTOM_NAME_SECTION 1)

# Global heap pool for Zephyr
set (WAMR_BUILD_GLOBAL_HEAP_POOL 1)

if (NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_SIZE)
  set (WAMR_BUILD_GLOBAL_HEAP_SIZE 131072) # 128 KB
endif ()

set (WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

include (${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

zephyr_library_named (wamr_lib)

# Include generated WASM app headers from ExternalProject output
zephyr_library_include_directories(
  ${CMAKE_BINARY_DIR}/wasm-apps/include
)

zephyr_library_sources (
  ${WAMR_RUNTIME_LIB_SOURCE}
  wamr_lib.c
)

if(TARGET zephyr_generated_headers)
  add_dependencies(wamr_lib zephyr_generated_headers)
endif()

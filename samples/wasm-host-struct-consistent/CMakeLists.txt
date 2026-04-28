# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)

include(CheckPIESupported)

project(wasm_host_struct_consistent C ASM)

string(TOLOWER ${CMAKE_HOST_SYSTEM_NAME} WAMR_BUILD_PLATFORM)
if(APPLE)
  add_definitions(-DBH_PLATFORM_DARWIN)
endif()

set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

# --- WAMR build target auto-detection ---
if(NOT DEFINED WAMR_BUILD_TARGET)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
    set(WAMR_BUILD_TARGET "AARCH64")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    set(WAMR_BUILD_TARGET "RISCV64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WAMR_BUILD_TARGET "X86_64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(WAMR_BUILD_TARGET "X86_32")
  else()
    message(SEND_ERROR "Unsupported build target platform!")
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
if(NOT MSVC)
  set(WAMR_BUILD_LIBC_WASI 1)
endif()

if(NOT MSVC)
  if(NOT (CMAKE_C_COMPILER MATCHES ".*clang.*" OR CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
  endif()
endif()

set(WAMR_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

add_library(vmlib ${WAMR_RUNTIME_LIB_SOURCE})

# --- Struct layout check (runs at configure time) ---
#
# Auto-detect wasi-sdk for the struct layout checker
if(NOT DEFINED WASI_SDK_DIR)
  foreach(_dir /opt/wasi-sdk /opt/wasi-sdk-25.0-x86_64-linux
               /opt/wasi-sdk-29.0-x86_64-linux $ENV{WASI_SDK_DIR})
    if(EXISTS "${_dir}/bin/clang")
      set(WASI_SDK_DIR "${_dir}")
      break()
    endif()
  endforeach()
endif()

# Determine native flags for struct layout check
# On x86-64 hosts checking 32-bit layout, pass -m32
if(WAMR_BUILD_TARGET STREQUAL "X86_32" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(STRUCT_CHECK_NATIVE_FLAGS "-m32")
else()
  set(STRUCT_CHECK_NATIVE_FLAGS "")
endif()

# Add -fshort-enums to demonstrate enum size mismatch:
# native gcc -fshort-enums packs enums to the smallest type (1 byte here),
# while wasm32-clang always uses 4-byte enums — triggers mismatch on x86-64.
set(STRUCT_CHECK_NATIVE_FLAGS "${STRUCT_CHECK_NATIVE_FLAGS} -fshort-enums")
add_compile_options(-fshort-enums)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_struct_layout.cmake)
check_wasm_struct_layout(
  VERBOSE      TRUE
  SOURCE       ${CMAKE_CURRENT_SOURCE_DIR}/src/native_impl.c
  NATIVE_CC    ${CMAKE_C_COMPILER}
  NATIVE_FLAGS "${STRUCT_CHECK_NATIVE_FLAGS}"
  WASI_SDK     "${WASI_SDK_DIR}"
)

# --- Application ---
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/shared
)
include(${SHARED_DIR}/utils/uncommon/shared_uncommon.cmake)

add_executable(struct_check src/main.c src/native_impl.c ${UNCOMMON_SHARED_SOURCE})

check_pie_supported()
set_target_properties(struct_check PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(APPLE)
  target_link_libraries(struct_check vmlib -lm -ldl -lpthread)
else()
  target_link_libraries(struct_check vmlib -lm -ldl -lpthread -lrt)
endif()

# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)

include(ExternalProject)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(coredump_debug LANGUAGES C)

# Select which WASM crash app to embed (oob or stackoverflow)
set(CRASH_APP "oob" CACHE STRING "WASM crash app to embed: oob or stackoverflow")

if(CRASH_APP STREQUAL "oob")
  add_definitions(-DCRASH_APP_OOB)
  message(STATUS "Crash app: out-of-bounds memory access")
elseif(CRASH_APP STREQUAL "stackoverflow")
  add_definitions(-DCRASH_APP_STACKOVERFLOW)
  message(STATUS "Crash app: stack overflow")
else()
  message(FATAL_ERROR "CRASH_APP must be 'oob' or 'stackoverflow', got: ${CRASH_APP}")
endif()

# --- Build WASM apps via ExternalProject ---
set(WASM_APPS_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/wasm-apps")

ExternalProject_Add(wasm_apps
  SOURCE_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/wasm-apps"
  BINARY_DIR    "${CMAKE_CURRENT_BINARY_DIR}/wasm-apps-build"
  INSTALL_DIR   "${WASM_APPS_INSTALL_DIR}"
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${WASM_APPS_INSTALL_DIR}
  BUILD_ALWAYS  TRUE
  STEP_TARGETS  install
)

add_subdirectory(lib-wamr-zephyr)

# Ensure WASM headers are generated before wamr_lib compiles
add_dependencies(wamr_lib wasm_apps-install)

target_link_libraries(app PRIVATE wamr_lib)

target_sources(app PRIVATE src/main.c)

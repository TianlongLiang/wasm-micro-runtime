# Copyright (C) 2026 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)
project(debug_tools_optimized_wasm_apps LANGUAGES NONE)

set(WAMR_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# --- wasi-sdk tool discovery ---
if(NOT DEFINED WASI_SDK_PATH)
    if(DEFINED ENV{WASI_SDK_PATH})
        set(WASI_SDK_PATH "$ENV{WASI_SDK_PATH}")
    else()
        set(WASI_SDK_PATH "/opt/wasi-sdk")
    endif()
endif()

set(WASI_SDK_CLANG "${WASI_SDK_PATH}/bin/clang")
set(WASI_SDK_STRIP "${WASI_SDK_PATH}/bin/llvm-strip")

if(NOT EXISTS "${WASI_SDK_CLANG}")
    message(FATAL_ERROR "wasi-sdk clang not found at ${WASI_SDK_CLANG}.")
endif()
if(NOT EXISTS "${WASI_SDK_STRIP}")
    message(FATAL_ERROR "llvm-strip not found at ${WASI_SDK_STRIP}.")
endif()

# --- binaryen (wasm-opt) discovery ---
if(NOT DEFINED BINARYEN_PATH)
    if(DEFINED ENV{BINARYEN_PATH})
        set(BINARYEN_PATH "$ENV{BINARYEN_PATH}")
    else()
        set(BINARYEN_PATH "/opt/binaryen")
    endif()
endif()

set(WASM_OPT "${BINARYEN_PATH}/bin/wasm-opt")
if(NOT EXISTS "${WASM_OPT}")
    message(FATAL_ERROR "wasm-opt not found at ${WASM_OPT}. Set BINARYEN_PATH.")
endif()

set(WASM_COMPILE_FLAGS
    --target=wasm32-wasi
    -Oz -g -flto
    -Wl,--export=app_main
)

# Build a wasm app from one or more source files.
# Usage: build_wasm_app(<app_name> <source1.c> [<source2.c> ...])
function(build_wasm_app APP_NAME)
    set(SOURCES ${ARGN})

    set(SOURCE_PATHS "")
    foreach(SRC ${SOURCES})
        list(APPEND SOURCE_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}")
    endforeach()

    set(WASM_INTERMEDIATE "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.wasm")
    set(WASM_DEBUG        "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.debug.wasm")
    set(WASM_PROD         "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.prod.wasm")

    # 1. Compile all sources together with -Oz -g -flto
    add_custom_command(
        OUTPUT "${WASM_INTERMEDIATE}"
        COMMAND ${WASI_SDK_CLANG} ${WASM_COMPILE_FLAGS}
                -o "${WASM_INTERMEDIATE}"
                ${SOURCE_PATHS}
        DEPENDS ${SOURCE_PATHS}
        COMMENT "[${APP_NAME}] Compiling -> ${APP_NAME}.wasm (intermediate, -Oz -g -flto)"
    )

    # 2. Debug companion (preserves DWARF + name section)
    add_custom_command(
        OUTPUT "${WASM_DEBUG}"
        COMMAND ${WASM_OPT} -Oz -g -o "${WASM_DEBUG}" "${WASM_INTERMEDIATE}"
        DEPENDS "${WASM_INTERMEDIATE}"
        COMMENT "[${APP_NAME}] wasm-opt -Oz -g -> ${APP_NAME}.debug.wasm (companion)"
    )

    # 3. Production (strip debug companion to guarantee identical code)
    add_custom_command(
        OUTPUT "${WASM_PROD}"
        COMMAND ${WASI_SDK_STRIP} --strip-all -o "${WASM_PROD}" "${WASM_DEBUG}"
        DEPENDS "${WASM_DEBUG}"
        COMMENT "[${APP_NAME}] strip -> ${APP_NAME}.prod.wasm (production)"
    )

    add_custom_target(${APP_NAME}_wasm ALL
        DEPENDS "${WASM_INTERMEDIATE}" "${WASM_DEBUG}" "${WASM_PROD}"
    )

    install(FILES "${WASM_DEBUG}" "${WASM_PROD}" DESTINATION wasm-apps)
endfunction()

build_wasm_app(oob oob_main.c oob_access.c)
build_wasm_app(stackoverflow stackoverflow_main.c stackoverflow_recurse.c)

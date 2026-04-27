# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)
project(coredump_debug_wasm_apps LANGUAGES NONE)

set(WAMR_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")

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
    message(FATAL_ERROR "wasi-sdk clang not found at ${WASI_SDK_CLANG}. "
        "Set WASI_SDK_PATH to your wasi-sdk installation.")
endif()
if(NOT EXISTS "${WASI_SDK_STRIP}")
    message(FATAL_ERROR "llvm-strip not found at ${WASI_SDK_STRIP}. "
        "Set WASI_SDK_PATH to your wasi-sdk installation.")
endif()

find_program(XXD xxd REQUIRED)

# Helper script for xxd header generation (avoids CMake shell escaping issues)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen_header.sh"
"#!/bin/sh
\"${XXD}\" -i \"$1\" | sed 's/unsigned char.*\\[\\]/unsigned char __aligned(4) wasm_test_file[]/' | sed 's/unsigned int.*//' > \"$2\"
")

set(WAMR_SYSROOT "${WAMR_ROOT_DIR}/wamr-sdk/app/libc-builtin-sysroot")
set(WAMR_DEFINED_SYMBOLS "${WAMR_SYSROOT}/share/defined-symbols.txt")

set(WASM_COMPILE_FLAGS
    --target=wasm32
    -O0 -g
    -z stack-size=4096
    -Wl,--initial-memory=65536
    --sysroot=${WAMR_SYSROOT}
    -Wl,--allow-undefined-file=${WAMR_DEFINED_SYMBOLS}
    -Wl,--no-entry
    -nostdlib
    -Wl,--export=app_main
)

# --- per-app build function ---
function(build_wasm_app SOURCE_FILE)
    get_filename_component(APP_NAME ${SOURCE_FILE} NAME_WLE)

    set(WASM_UNSTRIPPED "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.wasm")
    set(WASM_STRIPPED   "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.stripped.wasm")
    set(HEADER_FILE     "${CMAKE_CURRENT_BINARY_DIR}/test_wasm_${APP_NAME}.h")

    # 1. Compile unstripped (with -g debug info)
    add_custom_command(
        OUTPUT "${WASM_UNSTRIPPED}"
        COMMAND ${WASI_SDK_CLANG} ${WASM_COMPILE_FLAGS}
                -o "${WASM_UNSTRIPPED}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}"
        COMMENT "Compiling ${SOURCE_FILE} -> ${APP_NAME}.wasm (unstripped)"
    )

    # 2. Strip debug info for embedding
    add_custom_command(
        OUTPUT "${WASM_STRIPPED}"
        COMMAND ${WASI_SDK_STRIP} --strip-debug
                -o "${WASM_STRIPPED}"
                "${WASM_UNSTRIPPED}"
        DEPENDS "${WASM_UNSTRIPPED}"
        COMMENT "Stripping ${APP_NAME}.wasm -> ${APP_NAME}.stripped.wasm"
    )

    # 3. Generate C header via xxd
    add_custom_command(
        OUTPUT "${HEADER_FILE}"
        COMMAND sh "${CMAKE_CURRENT_BINARY_DIR}/gen_header.sh"
                "${WASM_STRIPPED}" "${HEADER_FILE}"
        DEPENDS "${WASM_STRIPPED}"
        COMMENT "Generating test_wasm_${APP_NAME}.h"
    )

    add_custom_target(${APP_NAME}_wasm ALL
        DEPENDS "${WASM_UNSTRIPPED}" "${WASM_STRIPPED}" "${HEADER_FILE}"
    )

    install(FILES "${WASM_UNSTRIPPED}" "${WASM_STRIPPED}" DESTINATION wasm)
    install(FILES "${HEADER_FILE}" DESTINATION include)
endfunction()

build_wasm_app(oob.c)
build_wasm_app(stackoverflow.c)

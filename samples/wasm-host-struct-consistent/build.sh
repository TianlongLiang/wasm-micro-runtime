#!/bin/bash

# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Usage: ./build.sh [--target X86_32|X86_64]
#   Builds both the host application and the WASM app.
#   Default target: auto-detected by CMake.

CURR_DIR=$PWD
WAMR_DIR=${PWD}/../..
OUT_DIR=${PWD}/out
WASM_APPS=${PWD}/wasm-app

TARGET=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --target)
            TARGET="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--target X86_32|X86_64]"
            exit 1
            ;;
    esac
done

CMAKE_EXTRA_ARGS=""
if [ -n "$TARGET" ]; then
    CMAKE_EXTRA_ARGS="-DWAMR_BUILD_TARGET=${TARGET}"
fi

rm -rf ${OUT_DIR}
mkdir -p ${OUT_DIR}/wasm-app

echo "#####################build host application"
cd ${CURR_DIR}
mkdir -p cmake_build
cd cmake_build
cmake .. -DCMAKE_BUILD_TYPE=Debug ${CMAKE_EXTRA_ARGS}
make -j $(nproc)
if [ $? != 0 ]; then
    echo "BUILD_FAIL host app exit as $?"
    exit 2
fi
cp -a struct_check ${OUT_DIR}/

echo ""
echo "#####################build wasm app"
cd ${WASM_APPS}

/opt/wasi-sdk/bin/clang \
    --target=wasm32 -O2 \
    -I${CURR_DIR}/shared \
    -z stack-size=4096 \
    -Wl,--initial-memory=65536 \
    -Wl,--export=run \
    -Wl,--export=__heap_base,--export=__data_end \
    -Wl,--no-entry \
    -Wl,--allow-undefined \
    -nostdlib \
    -o ${OUT_DIR}/wasm-app/main.wasm main.c

if [ -f ${OUT_DIR}/wasm-app/main.wasm ]; then
    echo "build main.wasm success"
else
    echo "build main.wasm fail"
    exit 1
fi

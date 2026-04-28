#!/bin/bash

# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Usage: ./run.sh
#   Runs the struct_check host application against the WASM module.

OUT_DIR=${PWD}/out

if [ ! -f ${OUT_DIR}/struct_check ]; then
    echo "Error: ${OUT_DIR}/struct_check not found. Run ./build.sh first."
    exit 1
fi

if [ ! -f ${OUT_DIR}/wasm-app/main.wasm ]; then
    echo "Error: ${OUT_DIR}/wasm-app/main.wasm not found. Run ./build.sh first."
    exit 1
fi

cd ${OUT_DIR}
./struct_check -f wasm-app/main.wasm

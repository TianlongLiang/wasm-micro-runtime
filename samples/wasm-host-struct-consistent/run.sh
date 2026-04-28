#!/bin/bash

# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Usage: ./run.sh [--expect-mismatch]
#   Runs the struct_check host application against the WASM module.
#   --expect-mismatch: assert that the inconsistent struct causes errors
#                      (expected on x86-32, and x86-64 with -fshort-enums)

OUT_DIR=${PWD}/out
EXPECT_MISMATCH=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --expect-mismatch)
            EXPECT_MISMATCH=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--expect-mismatch]"
            exit 1
            ;;
    esac
done

if [ ! -f ${OUT_DIR}/struct_check ]; then
    echo "Error: ${OUT_DIR}/struct_check not found. Run ./build.sh first."
    exit 1
fi

if [ ! -f ${OUT_DIR}/wasm-app/main.wasm ]; then
    echo "Error: ${OUT_DIR}/wasm-app/main.wasm not found. Run ./build.sh first."
    exit 1
fi

cd ${OUT_DIR}
OUTPUT=$(./struct_check -f wasm-app/main.wasm)
echo "$OUTPUT"

# Extract the final "run() returned: N" line
RETURN_VAL=$(echo "$OUTPUT" | grep "run() returned:" | grep -oP '\d+' | head -1)

if [ "$EXPECT_MISMATCH" = true ]; then
    if [ "$RETURN_VAL" = "0" ] || [ -z "$RETURN_VAL" ]; then
        echo ""
        echo "ASSERTION FAILED: expected mismatch errors (non-zero return), got ${RETURN_VAL:-none}"
        exit 1
    fi
    echo ""
    echo "OK: mismatch detected as expected (returned ${RETURN_VAL})"
else
    if [ "$RETURN_VAL" != "0" ]; then
        echo ""
        echo "UNEXPECTED: run() returned ${RETURN_VAL} errors"
        exit 1
    fi
fi

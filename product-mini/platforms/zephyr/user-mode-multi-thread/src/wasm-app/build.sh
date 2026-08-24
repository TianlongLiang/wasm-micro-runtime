#!/bin/sh

# Copyright (C) 2026 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../../../../.." && pwd)
sysroot="$repo_root/wamr-sdk/app/libc-builtin-sysroot"
build_dir="$script_dir/build"

cd "$script_dir"

/opt/wasi-sdk/bin/clang --sysroot="$sysroot" -O2 -nostdlib -pthread \
    -z stack-size=4096 -Wl,--initial-memory=65536,--max-memory=131072 \
    -Wl,--shared-memory,--no-entry,--strip-all \
    -Wl,--export=main,--export=__main_argc_argv \
    -Wl,--export=__data_end,--export=__heap_base \
    -Wl,--allow-undefined -o test.wasm main.c

rm -rf "$build_dir"
cmake -S "$repo_root/test-tools/binarydump-tool" -B "$build_dir"
cmake --build "$build_dir"

"$build_dir/binarydump" -o ../test_wasm.h -n wasm_test_file test.wasm

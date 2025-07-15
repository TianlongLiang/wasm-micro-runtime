#!/bin/bash
set -e
CUR_DIR=$(pwd)
WAMR_DIR=${CUR_DIR}/../..
WASM_APPS=${CUR_DIR}/wasm-apps
rm -rf build
mkdir build
mkdir -p build/wasm-apps

# build native host
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build

# build wasm app
clang --target=wasm32 -O0 -nostdlib -Wl,--no-entry \
      -Wl,--export=call_add -Wl,--export=call_sub \
      ${WASM_APPS}/acl_app.c -o ./build/wasm-apps/acl_app.wasm

# build aot file if wamrc is available
WAMRC=${WAMR_DIR}/wamr-compiler/build/wamrc
if [ -x ${WAMRC} ]; then
    ${WAMRC} -o ./build/wasm-apps/acl_app.aot ./build/wasm-apps/acl_app.wasm
fi

echo "Build done"


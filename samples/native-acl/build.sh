#!/bin/bash
set -e
CUR_DIR=$(pwd)
WAMR_DIR=${CUR_DIR}/../..
rm -rf build

# build native host
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build

# build aot file if wamrc is available
WAMRC=${WAMR_DIR}/wamr-compiler/build/wamrc
if [ -x ${WAMRC} ]; then
    ${WAMRC} -o ./build/wasm-app/acl_app.aot ./build/wasm-app/acl_app.wasm
fi

echo "Build done"


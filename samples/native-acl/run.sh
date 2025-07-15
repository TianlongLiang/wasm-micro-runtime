#!/bin/bash
# Run the native_acl sample. Pass "--aot" to run the AOT compiled module.
cd build || exit

if [ "$1" = "--aot" ]; then
    ./native_acl --aot
else
    ./native_acl
fi

cd ..
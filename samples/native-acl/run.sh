#!/bin/bash
# Run the native_acl sample. Pass "--aot" to run the AOT compiled module.
if [ "$1" = "--aot" ]; then
    ./build/native_acl --aot
else
    ./build/native_acl
fi

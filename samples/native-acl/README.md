# "native-acl" sample introduction

This sample demonstrates the native API access control feature. It registers two native functions `native_add` and `native_sub` and configures an access control list using the helper API `wasm_runtime_create_native_symbol_acl`. The ACL must later be released with `wasm_runtime_destroy_native_symbol_acl`. The allow list is passed in the `LoadArgs` to `wasm_runtime_load_ex` so that the loaded WebAssembly module can only import `native_add`.

When the module tries to resolve `native_sub` the instantiation will fail and the error will be reported.

The build script will also produce an AOT version of the wasm file using `wamrc`. Run the sample with `--aot` to execute this compiled binary.

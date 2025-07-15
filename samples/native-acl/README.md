# "native-acl" sample introduction

This sample demonstrates the native API access control feature. It registers two native functions `native_add` and `native_sub` and configures an access control list in the `LoadArgs` passed to `wasm_runtime_load_ex` so that the loaded WebAssembly module can only import `native_add`.

When the module tries to resolve `native_sub` the instantiation will fail and the error will be reported.

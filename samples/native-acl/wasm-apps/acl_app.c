__attribute__((import_module("env"), import_name("native_add")))
int native_add(int a, int b);
__attribute__((import_module("env"), import_name("native_sub")))
int native_sub(int a, int b);

int call_add(int a, int b) { return native_add(a, b); }
int call_sub(int a, int b) { return native_sub(a, b); }

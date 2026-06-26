# How to use WAMR with Zephyr in user mode

This example demonstrates how to build and run a WebAssembly application in user mode on Zephyr.

> Note: The user mode is not supported on all Zephyr boards. Please refer to the Zephyr documentation for more information.

## Build matrix

This sample supports multiple build configurations via two flags:

| `WAMR_BUILD_ZEPHYR_USERMODE_MT` | `USER_MODE_MULTITHREAD` | Outcome |
|---|---|---|
| 1 (default) | ON (default) | Worker-pool + bh_queue demo |
| 1 | OFF | Single user-mode thread + bh_queue round trip |
| 0 | OFF | ST demo starts, faults at `k_mutex_init` inside `bh_queue_create` |
| 0 | ON | Rejected by CMake |

See `docs/zephyr-usermode-internals.md` for why the `WAMR_BUILD_ZEPHYR_USERMODE_MT` flag is needed.

**Note:** There is no separate `user-mode-app/` sample in this tree. During development, an attempt was made to create a "pure Zephyr-app shape" sample (WAMR sources compiled directly into the app target, like `simple/`). However, under `CONFIG_USERSPACE`, any global that a user-mode thread accesses must live inside a memory partition the thread has access to. Achieving this requires `zephyr_library_app_memory(wamr_partition)`, which means WAMR must be built as a `zephyr_library` — the same structure this sample already has. See `docs/zephyr-usermode-internals.md` section "Why a Pure Zephyr-App User-Mode Sample Was Not Added" for the full technical explanation.

## Setup

Please refer to the [previous WAMR Zephyr README.md](../simple/README.md) for general Zephyr setup instructions.

And refer to [official documentation of Zephyr user mode](https://docs.zephyrproject.org/latest/kernel/usermode/index.html) for more information about Zephyr user mode.

### Enable user mode

To enable Zephyr user mode, set the `CONFIG_USERSPACE` option to yes in the Zephyr configuration.

```conf
CONFIG_USERSPACE=y
```

And link the WAMR runtime as a separate library in CMakelists.txt.

```cmake
...WAMR CMake set up...

zephyr_library_named (wamr_lib)

zephyr_library_sources (
  ${WAMR_RUNTIME_LIB_SOURCE} 
  wamr_lib.c
)

zephyr_library_app_memory (wamr_partition)
```

The `wamr_partition` is a memory partition that will be granted to the WAMR runtime. It is defined in the Zephyr application code.

```C
K_APPMEM_PARTITION_DEFINE(wamr_partition);
```

When creating a Zephyr thread, set the thread option to `K_USER` and the timeout to `K_FOREVER`. This can ensure that the `wamr_partition` is granted access to the thread before starting it with `k_thread_start`.

### Advantage of using WAMR runtime in Zephyr user mode thread

In a user-mode Zephyr thread, the application can only access a restricted partition of memory it granted to. It creates a sandbox for the WAMR runtime to run in, and the WAMR runtime can only access that memory space, meaning that all global variables in the WAMR runtime and both runtime and wasm app heap memory will be allocated from it. In this way, an extra layer of security is added to the wasm application on top of the wasm sandbox provided by WAMR.

### Using a pre-built WAMR library in user mode

If the WAMR library is pre-built as a static archive (`.a` file) rather than
compiled inline via `add_subdirectory`, the library's global variables still
need to be placed into the `app_smem` section so they are accessible from the
user-mode thread. This is useful when you want to treat the WAMR runtime as a
binary dependency copied from an external build.

#### How `zephyr_library_app_memory` works internally

`zephyr_library_app_memory(partition)` is a thin wrapper that appends metadata
to a CMake target property:

```cmake
# zephyr/cmake/modules/extensions.cmake
set_property(TARGET zephyr_property_target
             APPEND PROPERTY COMPILE_OPTIONS
             "-l" <library_filename> "<partition_name>")
```

Zephyr's build system passes this metadata as `-l libname.a partition` arguments
to `gen_app_partitions.py`, which generates a linker script fragment with
wildcard patterns that collect the library's `.data` and `.bss` sections into
the named partition:

```ld
"*libwamr_lib.a:*"(.data .data.* .sdata .sdata.*)
"*libwamr_lib.a:*"(.bss .bss.* .sbss .sbss.* COMMON COMMON.*)
```

#### Using the built-in `WAMR_USE_PREBUILT_LIB` option

This sample's CMakeLists.txt supports a `WAMR_USE_PREBUILT_LIB` option. When
enabled, the library is still compiled from source under `lib-wamr-zephyr/`,
but partition registration bypasses `zephyr_library_app_memory()` and uses the
manual `set_property()` approach instead. This demonstrates the same integration
path you would use with an externally built `.a` file.

Build from source with `zephyr_library_app_memory` (default):

```shell
west build -b qemu_x86 . -p always
```

Build from source with pre-built library partition registration:

```shell
west build -b qemu_x86 . -p always -- -DWAMR_USE_PREBUILT_LIB=1
```

The application code (`main.c`) is unchanged in both cases — define the
partition with `K_APPMEM_PARTITION_DEFINE(wamr_partition)`, set up the memory
domain, and create a user-mode thread as usual.

#### Applying this to your own project

To use a pre-built WAMR library in a standalone Zephyr application, add the
following to your CMakeLists.txt:

```cmake
# Import the pre-built library
add_library(wamr_lib STATIC IMPORTED GLOBAL)
set_target_properties(wamr_lib PROPERTIES
  IMPORTED_LOCATION /path/to/libwamr_lib.a
)

# Tell gen_app_partitions.py to place this library's globals into wamr_partition.
# This replicates what zephyr_library_app_memory(wamr_partition) does for
# libraries built through zephyr_library_named().
set_property(TARGET zephyr_property_target
             APPEND PROPERTY COMPILE_OPTIONS
             "-l" "libwamr_lib.a" "wamr_partition")

# Link it to the app
target_link_libraries(app PRIVATE wamr_lib)
```

#### Notes

- The library filename in the `-l` argument must match the archive filename
  that the linker sees (e.g. `libwamr_lib.a`).
- The pre-built library must be compiled with the same Zephyr toolchain and
  flags (architecture, sysroot, etc.) as the application.
- For Zephyr 4.x, if building the library inline via `add_subdirectory`, add
  `add_dependencies(wamr_lib zephyr_generated_headers)` to avoid build race
  conditions with generated headers like `heap_constants.h`.

### Multi-threaded user-mode demo

This sample demonstrates WAMR running entirely in user mode with multi-threaded
bh_queue communication. The architecture:

```
kernel main()                          (supervisor mode)
  ├── set up memory domain (wamr_partition + z_libc_partition)
  ├── os_thread_env_init_for_usermode()  grant MPU stack access
  └── spawn wamr_main thread           (K_USER, K_FOREVER → start after setup)

iwasm_main()                           (user mode, in wamr_lib.c)
  ├── wasm_runtime_full_init()          pool allocator, heap lock via k_object_alloc
  ├── wasm_runtime_load()               load WASM module
  ├── bh_queue_create()                 queue lock + condvar via k_object_alloc
  └── os_thread_create() × N workers   (K_USER | K_INHERIT_PERMS, automatic)
        ├── wasm_runtime_instantiate()  each worker gets its own WASM instance
        ├── wasm_application_execute_main()  run the WASM app
        ├── bh_post_msg() × M          send messages to shared bh_queue
        └── wasm_runtime_deinstantiate()

iwasm_main() consumer loop             (user mode)
  ├── bh_get_msg(BHT_WAIT_FOREVER)     blocks on k_condvar
  └── os_thread_join() × N             wait for workers to finish
```

All WAMR code — runtime init, module loading, WASM execution, bh_queue
operations, and worker thread creation — runs in user mode. The only supervisor
code is in `main.c`: memory domain setup and one-time stack access grants.

## Platform-internals reference

For the design rationale (kobject registration gap, K_INHERIT_PERMS, linker
hint mechanism, join-race fix), see [`docs/zephyr-usermode-internals.md`](../../../../docs/zephyr-usermode-internals.md).

### Example Targets

#### qemu_x86 (Zephyr 4.x with Zephyr SDK 1.0+)

Build for the `qemu_x86` board (32-bit x86, the default `WAMR_BUILD_TARGET`):

```shell
west build -b qemu_x86 . -p always
```

To use the pre-built library approach instead:

```shell
west build -b qemu_x86 . -p always -- -DWAMR_USE_PREBUILT_LIB=1
```

Run on QEMU using `west`:

```shell
west build -t run
```

> Press `CTRL+a, x` to exit QEMU.

Expected output (multi-thread, flag=1, default):

```
Hello world!
buf ptr: 0x1458
buf: 1234
Hello world!
buf ptr: 0x1458
buf: 1234
*** Booting Zephyr OS build v4.4.0-rc2 ***
=== WAMR User-Mode Demo ===
bh_queue created (user mode)

Starting 2 workers (WASM + bh_queue, 5 msgs each):
  worker 0: running WASM app
  [send] worker 0: msg 0 "w0-msg0"
  [recv] #0 from worker 0 seq 0 "w0-msg0"
  worker 1: running WASM app
  [send] worker 1: msg 0 "w1-msg0"
  [recv] #1 from worker 1 seq 0 "w1-msg0"
  ...
Total: sent 10, received 10
=== Demo complete ===
```

#### Single-thread mode (flag=1, USER_MODE_MULTITHREAD=OFF)

To run the single-thread variant (one user-mode thread + bh_queue round trip):

```shell
west build -b qemu_x86 . -p always -- -DUSER_MODE_MULTITHREAD=OFF
west build -t run
```

Expected output:

```
*** Booting Zephyr OS build v4.4.0-rc2 ***
=== WAMR User-Mode Demo (single-thread) ===
bh_queue created (user mode)
  [recv] worker 0 seq 0 "app-st-msg"
=== Demo complete ===
```

#### Demonstrating the failure mode (flag=0, USER_MODE_MULTITHREAD=OFF)

To see what happens when `WAMR_BUILD_ZEPHYR_USERMODE_MT=0` (flag off) with
single-thread mode:

```shell
west build -b qemu_x86 . -p always -- -DWAMR_BUILD_ZEPHYR_USERMODE_MT=0 -DUSER_MODE_MULTITHREAD=OFF
west build -t run
```

Expected output (fault at `k_mutex_init` inside `bh_queue_create`):

```
*** Booting Zephyr OS build v4.4.0-rc2 ***
=== WAMR User-Mode Demo (single-thread) ===
[00:00:00.020,000] <err> os: 0x... is not a valid k_mutex
[00:00:00.020,000] <err> os: address is not a known kernel object
[00:00:00.020,000] <err> os: syscall z_vrfy_k_mutex_init failed check: access denied
```

The demo starts but faults when attempting to create the bh_queue because the
mutex is not registered in the kernel object table. This demonstrates why
`WAMR_BUILD_ZEPHYR_USERMODE_MT=1` is required for user-mode bh_queue use.

Attempting to build with `WAMR_BUILD_ZEPHYR_USERMODE_MT=0` and
`USER_MODE_MULTITHREAD=ON` will be rejected by CMake with an error message.

#### qemu_arc (ARC HS)

```shell
west build -b qemu_arc/qemu_arc_hs . -p always -- -DWAMR_BUILD_TARGET=ARC
west build -t run
```

#### qemu_x86_tiny (older Zephyr / manual QEMU)

Build for the `qemu_x86_tiny` board:

```shell
west build -b qemu_x86_tiny . -p always -- -DWAMR_BUILD_TARGET=X86_32
```

Run QEMU manually:

```shell
qemu-system-i386 -m 32 -cpu qemu32,+nx,+pae -machine pc \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -no-reboot -nographic -net none -pidfile qemu.pid \
  -chardev stdio,id=con,mux=on -serial chardev:con \
  -mon chardev=con,mode=readline \
  -icount shift=5,align=off,sleep=off -rtc clock=vm \
  -kernel ./build/zephyr/zephyr.elf
```

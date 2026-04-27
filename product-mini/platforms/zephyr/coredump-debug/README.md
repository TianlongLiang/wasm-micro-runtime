# Zephyr Coredump Debug Demo

Demonstrates end-to-end crash debugging of a WASM app running on Zephyr,
combining three debugging layers:

- **WAMR call stack dump** — shows which WASM function crashed and the
  call chain leading to it (function names + bytecode offsets).
- **addr2line symbolication** — resolves WASM bytecode offsets to source
  file, line, and column using DWARF debug info from the unstripped WASM
  binary.
- **Zephyr native coredump** — dumps system-level state (registers, memory)
  for offline analysis with GDB.

Together they provide full visibility: addr2line tells you *which source line*
crashed, WAMR tells you *which WASM function* and call chain, and the Zephyr
coredump tells you *what the native runtime state* looked like at the point of
failure.

## Prerequisites

- Zephyr SDK 1.0+ and Zephyr workspace (see [Zephyr Getting Started](https://docs.zephyrproject.org/latest/develop/getting_started/index.html))
- wasi-sdk at `WASI_SDK_PATH` or `/opt/wasi-sdk` (required for building WASM apps)
- wabt at `WABT_PATH` or `/opt/wabt` (required for addr2line symbolication)
- Python 3
- `xxd` (typically bundled with `vim`)

## How it works

### Build pipeline

`west build` automatically compiles the WASM apps using wasi-sdk via an
ExternalProject:

1. Compiles each WASM source with `-g` debug info → `<name>.wasm` (unstripped)
2. Strips debug sections → `<name>.stripped.wasm` (small, for embedding)
3. Generates C header via `xxd` → `test_wasm_<name>.h`
4. The Zephyr app links against the stripped WASM embedded in the header

### WAMR call stack dump

When `WAMR_BUILD_DUMP_CALL_STACK=1` is enabled (set in
`lib-wamr-zephyr/CMakeLists.txt`), WAMR automatically prints the WASM-level
call stack whenever a WASM trap occurs (e.g., out-of-bounds memory access,
stack overflow). The output shows function names and bytecode offsets:

```
#00: 0x0072 - do_bad_access
#01: 0x0082 - trigger_oob
#02: 0x008c - app_main
```

Function names are available because `WAMR_BUILD_CUSTOM_NAME_SECTION=1` is
also enabled, which loads names from the WASM binary's custom name section.

### addr2line symbolication

The `capture_coredump.sh` script uses WAMR's
[addr2line.py](../../../../test-tools/addr2line/addr2line.py) to resolve
bytecode offsets to source locations using the unstripped WASM binary's
DWARF debug info:

```
0: do_bad_access
        at oob.c:12:5
1: trigger_oob
        at oob.c:21:5
2: app_main
        at oob.c:27:5
```

### Zephyr coredump

When `CONFIG_DEBUG_COREDUMP=y` is set in `prj.conf`, Zephyr's fatal error
handler dumps the CPU registers and memory regions to the console as hex-encoded
data, bracketed by `#CD:BEGIN#` and `#CD:END#` markers.

This demo uses the **logging backend** (`CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING=y`),
which prints the coredump hex to the serial console. This is the simplest
backend — no flash partition or special hardware needed.

### The three layers together

When the WASM app traps:

1. WAMR catches the trap and prints the WASM call stack (function names + offsets)
2. The host code calls `k_panic()` to trigger a Zephyr fatal error
3. Zephyr's fatal handler dumps registers and memory as `#CD:` hex lines
4. The capture script extracts the call stack and runs addr2line for source-level info
5. The system halts

## Crash apps

Two WASM apps are included, selected at build time via `-DCRASH_APP=`:

| App | Trigger | Purpose |
|-----|---------|---------|
| `oob` | Out-of-bounds memory write | Demonstrates WASM trap on invalid memory access |
| `stackoverflow` | Deep recursion | Demonstrates WASM stack exhaustion |

## Quick start

### Build

```shell
# Build with out-of-bounds crash app (default)
west build -b qemu_x86 . -p always -- -DCRASH_APP=oob

# Or build with stack overflow crash app
west build -b qemu_x86 . -p always -- -DCRASH_APP=stackoverflow
```

This automatically compiles the WASM apps, generates headers, and builds the
Zephyr firmware. wasi-sdk must be installed at `WASI_SDK_PATH` or `/opt/wasi-sdk`.

### Run and capture

```shell
bash scripts/capture_coredump.sh
```

The script runs QEMU, captures the full console output to `build/qemu_output.log`,
extracts the WAMR call stack, runs addr2line for source-level symbolication,
and extracts the Zephyr coredump hex.

Or run manually:

```shell
west build -t run
# Press CTRL+a, x to exit QEMU after the crash
```

### What you'll see

```
Coredump debug demo: starting WAMR...
Calling WASM app_main (expect crash)...

#00: 0x0072 - do_bad_access
#01: 0x0082 - trigger_oob
#02: 0x008c - app_main

WASM exception: Exception: out of bounds memory access
Triggering Zephyr coredump via k_panic()...

<err> os: >>> ZEPHYR FATAL ERROR 4: Kernel panic on CPU 0
<err> coredump: #CD:BEGIN#
<err> coredump: #CD:5a4502000100050004000000
...
<err> coredump: #CD:END#
<err> os: Halting system
```

The capture script output includes the symbolicated call stack:

```
=== WASM Symbolicated Call Stack ===
0: do_bad_access
        at oob.c:12:5
1: trigger_oob
        at oob.c:21:5
2: app_main
        at oob.c:27:5
```

### Environment variables

| Variable | Default | Used by |
|----------|---------|---------|
| `WASI_SDK_PATH` | `/opt/wasi-sdk` | Build (clang, llvm-strip) and symbolication (llvm-dwarfdump) |
| `WABT_PATH` | `/opt/wabt` | Symbolication (wasm-objdump) |

## Analyzing the Zephyr coredump with GDB

The coredump hex in the console log can be converted to a binary and loaded
into GDB for native-level debugging. Zephyr provides Python scripts for this
(no `west` subcommand — these are standalone scripts).

### Step 1: Parse the serial log hex into a binary

```shell
python3 ${ZEPHYR_BASE}/scripts/coredump/coredump_serial_log_parser.py \
    build/qemu_output.log build/coredump.bin
```

This reads the `#CD:BEGIN#` ... `#CD:END#` block and produces a binary file.

### Step 2: Start the coredump GDB server

```shell
python3 ${ZEPHYR_BASE}/scripts/coredump/coredump_gdbserver.py \
    build/zephyr/zephyr.elf build/coredump.bin
```

This starts a GDB-compatible server on port 1234 that serves the crashed
system's registers and memory to GDB.

### Step 3: Connect GDB (in another terminal)

Use the GDB from your Zephyr SDK:

```shell
# For qemu_x86 (32-bit x86):
${ZEPHYR_SDK_INSTALL_DIR}/gnu/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-gdb \
    build/zephyr/zephyr.elf

(gdb) target remote localhost:1234
(gdb) bt                    # native backtrace
(gdb) info registers        # CPU register state at crash
(gdb) frame 0               # inspect specific frame
(gdb) list                   # show source at crash point
```

Or as a one-liner using pipe mode (single terminal):

```shell
${ZEPHYR_SDK_INSTALL_DIR}/gnu/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-gdb \
    build/zephyr/zephyr.elf \
    -ex "target remote | python3 ${ZEPHYR_BASE}/scripts/coredump/coredump_gdbserver.py --pipe build/zephyr/zephyr.elf build/coredump.bin"
```

### What GDB tells you

The `bt` (backtrace) command in GDB shows the native C call stack in the WAMR
runtime at the point `k_panic()` was called. This complements the WAMR
WASM-level call stack:

- **WAMR call stack** answers: *which WASM function crashed and what was the
  WASM call chain?*
- **addr2line** answers: *which source file and line in the original C code?*
- **GDB native backtrace** answers: *which C function in the WAMR runtime
  handled the trap, and what was the native call chain?*

## References

- [Zephyr Coredump Documentation](https://docs.zephyrproject.org/latest/services/debugging/coredump.html)
- [WAMR Dump Call Stack Feature](../../../../doc/build_wamr.md#dump-call-stack-feature)
- [WAMR Debug Tools Sample](../../../../samples/debug-tools/README.md)
- [addr2line.py](../../../../test-tools/addr2line/addr2line.py)

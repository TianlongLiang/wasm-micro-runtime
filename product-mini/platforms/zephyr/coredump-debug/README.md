# Zephyr Coredump Debug Demo (Optimized WASM)

Demonstrates end-to-end crash debugging of **production-optimized** WASM
running on Zephyr. The WASM apps are compiled with `-Oz -flto` and post-build
optimized with `wasm-opt -Oz`, which aggressively inlines functions and
strips debug info. A "debug companion" binary built in parallel retains
DWARF inline info, enabling full call stack recovery offline.

Three debugging layers work together:

- **WAMR call stack dump** — reports function index + bytecode offset at
  the point of the WASM trap (no function names in the stripped binary).
- **Offline inline decode** — `decode_callstack.py` uses `llvm-addr2line -i`
  against the debug companion to reconstruct the full inlined call chain
  with source file, line, and column.
- **Zephyr native coredump** — dumps system-level state (registers, memory)
  for offline analysis with GDB.

## Prerequisites

- Zephyr SDK 1.0+ and Zephyr workspace ([Getting Started](https://docs.zephyrproject.org/latest/develop/getting_started/index.html))
- wasi-sdk at `WASI_SDK_PATH` or `/opt/wasi-sdk`
- binaryen (wasm-opt) at `BINARYEN_PATH` or `/opt/binaryen`
- wabt at `WABT_PATH` or `/opt/wabt`
- Python 3
- `xxd` (typically bundled with `vim`)

## How it works

### Build pipeline

`west build` compiles the WASM apps through a 4-step pipeline:

1. `clang -Oz -g -flto` compiles all source files together (whole-program
   via LTO for cross-translation-unit inlining) → `<name>.wasm` (intermediate)
2. `wasm-opt -Oz -g` → `<name>.debug.wasm` (debug companion — same
   optimized code, retains DWARF + name section)
3. `llvm-strip --strip-all` on the debug companion → `<name>.prod.wasm`
   (production — minimal, no debug info)
4. `xxd` on `.prod.wasm` → C header for embedding in firmware

The production binary is derived directly from the debug companion by
stripping. This **guarantees byte-identical code sections** between the two,
which is required for offset mapping to work correctly.

### Why multi-file sources?

The WASM apps use multiple source files (e.g., `oob_main.c` +
`oob_access.c`) to demonstrate that cross-translation-unit inlining
works correctly under `-Oz -flto` and that `llvm-addr2line -i` resolves
inlined functions back to their original source files.

### WAMR call stack dump

With `WAMR_BUILD_DUMP_CALL_STACK=1` enabled, WAMR prints the WASM-level
call stack on trap. Since the production binary has no name section, the
output shows function indices and bytecode offsets only:

```
#00: 0x0039 - $f0
```

### Offline inline decode

The `decode_callstack.py` tool converts these raw offsets into full
inlined call stacks using the debug companion:

```
#00: 0x0039 - $f0
  Inlined call stack:
    do_bad_access
        at oob_access.c:11:5
    trigger_oob
        at oob_main.c:17:5
    app_main
        at oob_main.c:23:5
```

**How it works:**
1. Reads the Code section start offset from the debug companion
2. Converts: `dwarf_addr = runtime_file_offset - code_section_start - 1`
3. Calls `llvm-addr2line -e companion.debug.wasm -f -i` to resolve
   the full inline chain from DWARF `DW_TAG_inlined_subroutine` entries

The `-1` is needed because WAMR reports the instruction pointer *after*
advancing past the faulting instruction (similar to a return address in
native code). The DWARF ranges cover the instruction itself, so without
the adjustment the address lands outside the inlined subroutine range.

### Zephyr coredump

When `CONFIG_DEBUG_COREDUMP=y` is set, Zephyr dumps CPU registers and
memory to the console as hex-encoded data for offline GDB analysis.

## Crash apps

Two multi-file WASM apps are included, selected at build time:

| App | Files | Trigger |
|-----|-------|---------|
| `oob` | `oob_main.c`, `oob_access.c` | Out-of-bounds memory write |
| `stackoverflow` | `stackoverflow_main.c`, `stackoverflow_recurse.c` | Deep recursion |

## Quick start

### Build

```shell
# Build with out-of-bounds crash app (default)
west build -b qemu_x86 . -p always -- -DCRASH_APP=oob

# Or build with stack overflow crash app
west build -b qemu_x86 . -p always -- -DCRASH_APP=stackoverflow
```

### Run and capture

```shell
bash scripts/capture_coredump.sh
```

The script runs QEMU, captures console output, extracts the WAMR call
stack, decodes it using the debug companion, and extracts the Zephyr
coredump hex.

Or run manually:

```shell
west build -t run
# Press CTRL+a, x to exit QEMU after the crash
```

### What you'll see

Runtime output (on device):
```
Coredump debug demo: starting WAMR...
Calling WASM app_main (expect crash)...

#00: 0x0039 - $f0

WASM exception: Exception: out of bounds memory access
Triggering Zephyr coredump via k_panic()...
```

Decoded output (offline, from capture script):
```
=== WASM Decoded Call Stack (inline resolution) ===
#00: 0x0039 - $f0
  Inlined call stack:
    do_bad_access
        at oob_access.c:11:5
    trigger_oob
        at oob_main.c:17:5
    app_main
        at oob_main.c:23:5
```

### Manual decode

To decode a call stack captured from real hardware (e.g., via UART):

```shell
# Save the WAMR call stack lines to a file
echo '#00: 0x0039 - $f0' > callstack.txt

# Decode using the debug companion
python3 ../../../../test-tools/decode-callstack/decode_callstack.py \
    --wasi-sdk /opt/wasi-sdk \
    --wabt /opt/wabt \
    --debug-wasm build/wasm-apps/wasm/oob.debug.wasm \
    callstack.txt
```

### Environment variables

| Variable | Default | Used by |
|----------|---------|---------|
| `WASI_SDK_PATH` | `/opt/wasi-sdk` | Build (clang, llvm-strip) and decode (llvm-addr2line) |
| `BINARYEN_PATH` | `/opt/binaryen` | Build (wasm-opt) |
| `WABT_PATH` | `/opt/wabt` | Decode (wasm-objdump) |

## Analyzing the Zephyr coredump with GDB

The coredump hex in the console log can be converted to a binary and
loaded into GDB for native-level debugging.

### Step 1: Parse the serial log hex into a binary

```shell
python3 ${ZEPHYR_BASE}/scripts/coredump/coredump_serial_log_parser.py \
    build/qemu_output.log build/coredump.bin
```

### Step 2: Start the coredump GDB server

```shell
python3 ${ZEPHYR_BASE}/scripts/coredump/coredump_gdbserver.py \
    build/zephyr/zephyr.elf build/coredump.bin
```

### Step 3: Connect GDB

```shell
${ZEPHYR_SDK_INSTALL_DIR}/gnu/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-gdb \
    build/zephyr/zephyr.elf

(gdb) target remote localhost:1234
(gdb) bt
(gdb) info registers
```

## The three debugging layers

| Layer | Answers | Requires |
|-------|---------|----------|
| WAMR call stack | Which WASM function trapped? What bytecode offset? | `WAMR_BUILD_DUMP_CALL_STACK=1` |
| Offline inline decode | Which source functions were inlined? File + line? | Debug companion + decode_callstack.py |
| GDB coredump | What was the native runtime state at the crash? | Zephyr coredump + zephyr.elf |

## Technical notes

### Why production is derived from the debug companion

`wasm-opt -Oz` without `-g` applies additional inlining passes that `-g`
inhibits (to preserve DWARF integrity). Running separate `wasm-opt` passes
for production and debug would produce structurally different binaries with
different code offsets, breaking the offline decode.

Instead, the production binary is created by stripping the debug companion
(`llvm-strip --strip-all`). This guarantees byte-identical code sections
and makes offset mapping unconditionally correct.

### LTO for cross-file inlining

The `-flto` flag enables link-time optimization, which allows the compiler
to inline functions across translation units. Without it, functions in
separate `.c` files would remain as separate WASM functions even under `-Oz`.
With LTO, the compiler sees all sources as one unit and inlines aggressively.

### Tail-call optimization and its effect on call stacks

`clang -flto -Oz` converts **tail-recursive** functions into loops at link
time. A tail call is one where the recursive call is the very last thing
the function does and its return value is passed through unchanged:

```c
return recurse(depth + 1);  // tail call — compiler can optimize
```

The compiler recognizes that the current stack frame is no longer needed
after the call, so it transforms the recursion into a loop that reuses
the same frame:

```wasm
func recurse:
  loop              ;; just a branch target, not a call
    ...             ;; function body
    local.set depth ;; update depth = depth + 1
    br 0            ;; jump back to top (no new stack frame)
  end
```

**Effect on call stack:** Since there is no `call` instruction, each
"recursive" iteration reuses the same WASM stack frame. When a trap
occurs inside the loop, the runtime reports only **one frame** for
`recurse` — not the full recursion depth:

```
#00: 0x0071 - $f1    ← recurse (one frame, regardless of depth)
#01: 0x0036 - app_main
```

This is correct behavior — it accurately reflects the optimized code
structure. The call stack shows the actual WASM frames that existed at
the point of the trap.

**To preserve the full recursive call chain** (e.g., for testing stack
overflow traps), make the call non-tail-recursive by using the return
value after the call:

```c
int r = recurse(depth + 1);
return r + buf[0];  // buf[0] forces the frame to stay alive
```

The compiler cannot discard the current frame before the call returns
(it still needs `buf[0]`), so it emits a real `call` instruction. Each
call pushes a new frame until the WASM operand stack overflows.

## References

- [Zephyr Coredump Documentation](https://docs.zephyrproject.org/latest/services/debugging/coredump.html)
- [WAMR Dump Call Stack Feature](../../../../doc/build_wamr.md#dump-call-stack-feature)
- [decode_callstack.py](../../../../test-tools/decode-callstack/decode_callstack.py)

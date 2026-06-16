# debug-tools-optimized — Debugging Production-Optimized WASM

This sample demonstrates symbolication of crashes in **production-optimized** WASM
binaries using the merged `addr2line.py`. The wasm apps are compiled with
`-Oz -g -flto` and post-processed with `wasm-opt -Oz -g`, then stripped to produce
minimal production binaries. A "debug companion" binary built in parallel retains
DWARF inline info, enabling source-level call stack recovery offline.

## Why this exists alongside `samples/debug-tools/`

The existing `samples/debug-tools/` sample uses `-O0 -g`: each function is preserved,
no inlining happens. This sample uses `-Oz -g -flto`, which:

- Aggressively inlines functions across translation units (cross-TU inlining via LTO)
- Strips the production binary to minimum size
- Tests `addr2line.py`'s inline expansion (`DW_TAG_inlined_subroutine` resolution)

If you only need to debug development builds, `samples/debug-tools/` is sufficient.
If you need to debug **shipped** binaries, this sample shows how.

## Build pipeline

```
clang -Oz -g -flto source1.c source2.c → <name>.wasm        (intermediate)
    └─ wasm-opt -Oz -g → <name>.debug.wasm                  (companion: code + DWARF + names)
        └─ llvm-strip --strip-all → <name>.prod.wasm        (production: code only)
```

## Why production is derived from the debug companion

`wasm-opt -Oz` (without `-g`) and `wasm-opt -Oz -g` produce **structurally different
binaries**: `-g` inhibits some inlining passes to preserve DWARF integrity. If we ran
them as separate pipelines, the production binary's code offsets would not match the
companion's DWARF address space — offline decode would silently break.

Instead, we run `wasm-opt -Oz -g` once and derive production by stripping the
companion (`llvm-strip --strip-all`). Custom sections (DWARF, names) live *after* the
code section in the WASM binary format, so stripping them doesn't shift code offsets.
This **guarantees byte-identical code** between production and companion.

## Why `-flto`

Without LTO, functions in separate `.c` files remain separate WASM functions even
under `-Oz`. With LTO, the compiler sees all sources as one unit and inlines
aggressively across files. This is what makes the `do_bad_access → trigger_oob →
app_main` chain collapse into a single WASM function with multiple inlined
subroutines.

## Why `recurse()` is non-tail-recursive

`stackoverflow_recurse.c` uses
`int r = recurse(depth + 1); return r + buf[0];` instead of
`return recurse(depth + 1);`. Tail calls (`return f(...)`) get converted to loops at
`-Oz -flto`, eliminating the recursion that we want to test. The non-tail form forces
a real `call` instruction so each iteration pushes a new frame.

## Prerequisites

- wasi-sdk at `WASI_SDK_PATH` or `/opt/wasi-sdk`
- binaryen at `BINARYEN_PATH` or `/opt/binaryen`
- wabt at `WABT_PATH` or `/opt/wabt`
- Python 3

## Quick start

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
cd ..
./symbolicate.sh oob
./symbolicate.sh stackoverflow
```

## Why `iwasm -f app_main` (and not just `iwasm <wasm>`)

The `symbolicate.sh` script invokes `iwasm -f app_main` instead of letting iwasm run
the default wasi `_start` entry. This matters for two reasons:

1. **Compiler folding**: Under `-Oz -flto`, when control reaches the OOB write through
   `_start → __wasi_main_void → main → app_main → ...`, LLVM observes the entire chain
   leading to undefined behavior and may rewrite it as `unreachable`. Calling
   `app_main` directly preserves the explicit OOB instruction and produces the
   expected `out of bounds memory access` exception.

2. **Cleaner trap point**: With `-f app_main`, the WAMR call stack starts at our app's
   entry, not deep inside wasi-libc startup, making the symbolication output more
   focused on user code.

## Expected output

### oob app

```
=== Running iwasm on oob.prod.wasm (expect crash) ===

#00: 0x0000 - app_main

Exception: out of bounds memory access

=== Captured call stack ===
#00: 0x0000 - app_main

=== Symbolicated call stack (using debug companion) ===
0: app_main
        at .../wasm-apps/oob_main.c:21  (offset=0 — function entry, no inline info)
```

The OOB trap happens at the **very first instruction** of the inlined-down `app_main`
(the `i32.store` to `0x7FFFFFFF`). When the trap occurs at function entry,
WAMR's `frame_ip` hasn't advanced yet, so the runtime reports `func_offset=0`. Without
a real instruction-pointer offset, `addr2line.py` can't disambiguate which inline
frame within `app_main` triggered the trap — it falls back to function-name lookup
and reports the `app_main` declaration line, with a note that inline info is
unavailable.

This is a **runtime-side limitation**, not a tooling issue: the address-zero pattern
prevents DWARF lookup of inline ranges. Adding any non-trapping instruction before
the OOB write (or having a deeper non-inlined call chain) would expose the full
inline expansion shown by the stackoverflow sample below.

### stackoverflow app

```
=== Running iwasm on stackoverflow.prod.wasm (expect crash) ===

#00: 0x1e0b - $f12
#01: 0x1dce - app_main

Exception: unreachable

=== Captured call stack ===
#00: 0x1e0b - $f12
#01: 0x1dce - app_main

=== Symbolicated call stack (using debug companion) ===
0: free
        at .../wasm-apps/stackoverflow_recurse.c:17
1: free
        at .../wasm-apps/stackoverflow_main.c:14
```

Stack overflow produces non-zero offsets (the runtime captures the ip of the call
instruction at each frame), so `addr2line.py` resolves source file and line number
correctly. The function name reads as `free` instead of `recurse`/`app_main` because
of a wasi-libc/DWARF aliasing artifact in the wasm-opt -g output — the **file:line
references are correct**.

## Manual decode

If you have a captured stack from another iwasm run (e.g., from a remote board or
saved log), you can symbolicate it directly:

```bash
python3 ../../test-tools/addr2line/addr2line.py \
    --wasi-sdk /opt/wasi-sdk \
    --wabt /opt/wabt \
    --wasm-file build/wasm-apps/oob.debug.wasm \
    /path/to/saved/call_stack.txt
```

## Environment variables

| Variable | Default | Used by |
|----------|---------|---------|
| `WASI_SDK_PATH` | `/opt/wasi-sdk` | Build (clang, llvm-strip) and decode (llvm-addr2line) |
| `BINARYEN_PATH` | `/opt/binaryen` | Build (wasm-opt) |
| `WABT_PATH` | `/opt/wabt` | Decode (wasm-objdump) |

## References

- [addr2line.py](../../test-tools/addr2line/addr2line.py)
- [WAMR Dump Call Stack Feature](../../doc/build_wamr.md#dump-call-stack-feature)
- [Zephyr coredump-debug sample](../../product-mini/platforms/zephyr/coredump-debug/) — same workflow on embedded
- [debug-tools sample](../debug-tools/) — non-optimized debug build for comparison

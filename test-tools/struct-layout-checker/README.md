# WAMR Struct Layout Checker

A build-time tool that detects struct layout mismatches between native host
code and WASM modules in WAMR.

## The Problem

When a WASM module passes a struct pointer to a native host API, the host
reads fields at offsets determined by the native compiler. If the native
compiler lays out the struct differently from wasm32-clang, the host reads
garbage — silently, with no compiler warning or runtime error.

This happens because wasm32-clang and native compilers disagree on:

| Type | wasm32-clang | x86-32 gcc | Effect |
|------|-------------|-----------|--------|
| `uint64_t` alignment | 8 bytes | 4 bytes | All fields after it shift |
| `double` alignment | 8 bytes | 4 bytes | All fields after it shift |
| `enum` size | always 4 bytes | 1 byte (`-fshort-enums`) | Size + offset shift |

For example, this struct has different layouts on wasm32 vs x86-32:

```c
struct device_info {
    uint8_t type;       // offset 0 on both
    uint64_t serial;    // offset 8 (wasm32) vs 4 (x86-32) — MISMATCH
};
```

The native host reads `serial` at offset 4, but WASM wrote it at offset 8.

## What This Tool Does

It compares struct layouts between native and WASM by reading DWARF debug
info from compiled object files, then reports mismatches with specific fix
suggestions:

```
=== struct device_report ===

  Field            Native             WASM               Match
  ──────────────── ────────────────── ────────────────── ─────
  id               off=0   sz=1       off=0   sz=1       OK
  info             off=8   sz=16      off=8   sz=16      OK
  voltage          off=24  sz=4       off=24  sz=4       OK
  status           off=28  sz=1       off=28  sz=4       MISMATCH (size)
  channel          off=29  sz=1       off=32  sz=1       MISMATCH (offset)
  calibration      off=32  sz=8       off=40  sz=8       MISMATCH (offset)
  sizeof           40                 48                 MISMATCH

  Suggested fixes for struct device_report:
    - 'status': enum is 1B native vs 4B wasm32 — replace with uint32_t
```

## Two Modes

### `source` — from C source code (build-time)

Parses WAMR `NativeSymbol` arrays in C source to automatically discover
which structs cross the WASM-host boundary. Compiles a probe with both
native gcc and wasm32-clang, then compares DWARF layouts.

```bash
python3 main.py source \
    --source src/native_impl.c \
    --native-cc gcc \
    --native-flags="-fshort-enums" \
    --wasi-sdk /opt/wasi-sdk \
    --verbose
```

This is what the CMake integration (`WAMR_CHECK_STRUCT_LAYOUT=ON`) uses.

### `binary` — from pre-built files (no source needed)

Extracts struct layouts directly from pre-built native (ELF) and WASM files
by reading their DWARF debug info. Auto-intersects struct names to find
shared types.

```bash
python3 main.py binary \
    --native build/my_app \
    --wasm build/module.wasm \
    --verbose
```

Both files must be compiled with `-g` (debug info).

## Project Structure

```
test-tools/struct-layout-checker/
├── main.py           CLI entry point with source and binary subcommands
├── discovery.py      NativeSymbol parsing, signature decoding, struct extraction
├── probe.py          Probe C file generation and dual compilation
├── dwarf.py          DWARF extraction from ELF (pyelftools) and WASM (llvm-dwarfdump)
├── compare.py        Field-by-field comparison, fix suggestions, recursive checking
├── tests/
│   ├── test_discovery.py     Unit tests for NativeSymbol parsing
│   ├── test_compare.py       Unit tests for layout comparison
│   ├── test_dwarf_parse.py   Unit tests for DWARF output parsing
│   ├── test_integration.py   End-to-end source subcommand test
│   └── test_binary_mode.py   End-to-end binary subcommand test
└── README.md
```

### How the modules connect

**`source` mode:**
```
discovery.py  →  probe.py  →  dwarf.py  →  compare.py
(find structs)  (compile)    (extract)    (compare)
```

**`binary` mode:**
```
dwarf.py  →  compare.py
(extract)    (compare)
```

### Module details

**`discovery.py`** — Parses `NativeSymbol` arrays in C source using regex.
Decodes WAMR signature strings (`"(*~)i"`) to find which parameters are
struct pointers. Matches pointer positions to C function parameter
declarations to extract struct type names. Flags `void*` parameters as
"unchecked" since their struct type is only known at runtime.

**`probe.py`** — Generates a minimal C file that declares one
`__attribute__((used))` global per struct, then compiles it with both native
and wasm32 compilers using `-g -c`. The `-c` (no linking) means unknown
symbols don't matter — we only need DWARF metadata.

**`dwarf.py`** — Extracts struct layouts from DWARF debug info. Auto-detects
format: ELF files are parsed with `pyelftools` (pure Python), WASM files
are parsed with `llvm-dwarfdump` (WASM embeds DWARF in custom sections that
pyelftools can't read). Follows DWARF type reference chains
(`DW_TAG_typedef` → `DW_TAG_base_type`) to resolve member sizes. Detects
nested structs for recursive checking.

**`compare.py`** — Compares native vs wasm32 layouts field-by-field (offset,
size, total struct size). Classifies mismatches (enum size, alignment,
cascade, nested struct) and generates specific fix suggestions. Collects
nested struct names for recursive checking. Output includes machine-readable
markers (`FIX_SUGGESTIONS_BEGIN`/`END`) for CMake integration.

**`main.py`** — CLI with `source` and `binary` subcommands. Orchestrates the
pipeline. The `binary` subcommand auto-intersects struct names from both
DWARF files and filters out common libc structs.

## Usage Examples

### Check WAMR native API structs from source

```bash
# Verbose output with full field-by-field tables
python3 main.py source \
    --source src/native_impl.c \
    --native-cc gcc \
    --wasi-sdk /opt/wasi-sdk \
    --verbose

# With -fshort-enums to detect enum size mismatches
python3 main.py source \
    --source src/native_impl.c \
    --native-cc gcc \
    --native-flags="-fshort-enums" \
    --wasi-sdk /opt/wasi-sdk

# Check specific structs only
python3 main.py source \
    --source src/native_impl.c \
    --native-cc gcc \
    --wasi-sdk /opt/wasi-sdk \
    --structs sensor_report,device_report

# x86-32 cross-check on a 64-bit host
python3 main.py source \
    --source src/native_impl.c \
    --native-cc gcc \
    --native-flags="-m32" \
    --wasi-sdk /opt/wasi-sdk
```

### Compare pre-built binaries

```bash
# Auto-discover shared structs between native and WASM
python3 main.py binary \
    --native build/my_app \
    --wasm build/module.wasm \
    --verbose

# Check specific structs only
python3 main.py binary \
    --native build/my_app \
    --wasm build/module.wasm \
    --structs my_config,my_state

# Include libc structs in comparison (normally filtered out)
python3 main.py binary \
    --native build/my_app \
    --wasm build/module.wasm \
    --no-filter

# Explicit llvm-dwarfdump path
python3 main.py binary \
    --native build/my_app \
    --wasm build/module.wasm \
    --llvm-dwarfdump /opt/wasi-sdk/bin/llvm-dwarfdump
```

### CMake integration

In your CMakeLists.txt:

```cmake
include(${WAMR_ROOT}/build-scripts/check_struct_layout.cmake)
check_wasm_struct_layout(
    SOURCE       ${CMAKE_CURRENT_SOURCE_DIR}/src/native_impl.c
    NATIVE_CC    ${CMAKE_C_COMPILER}
    NATIVE_FLAGS "-fshort-enums"
    WASI_SDK     /opt/wasi-sdk
    VERBOSE
)
```

Or enable for all WAMR native libraries:

```bash
cmake .. -DWAMR_CHECK_STRUCT_LAYOUT=ON
```

With `WAMR_CHECK_STRUCT_LAYOUT=ON`, mismatches cause a build failure
(`FATAL_ON_MISMATCH`). Without it, mismatches are warnings.

## Requirements

- Python 3.6+
- `pyelftools` (`pip install pyelftools`)

For `source` mode:
- Native C compiler (gcc or clang)
- wasi-sdk

For `binary` mode:
- `llvm-dwarfdump` (from wasi-sdk, LLVM, or system package)
- Both files compiled with `-g`

## Running Tests

```bash
cd test-tools/struct-layout-checker

# Unit tests only (no external tools needed)
python3 -m pytest tests/ -k "not integration and not binary" -v

# All tests (needs gcc + wasi-sdk)
python3 -m pytest tests/ -v
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All checked structs match |
| 1 | At least one mismatch found |
| 2 | Tool error (missing tools, compile failure, no DWARF info) |

# WASM-Host Struct Layout Consistency

This example demonstrates the problem of inconsistent struct layout between a WASM app and a native host, and provides a solution with both compile-time assertions and an automatic layout checker tool.

## The Problem

When a WASM app passes a struct pointer to a native host API, the host reads
fields at offsets determined by the native compiler. If the native compiler
lays out the struct differently from wasm32-clang, the host reads garbage.

This is a real problem on 32-bit platforms. wasm32-clang and native gcc
disagree on:

| Type | wasm32-clang | x86-32 / ARC gcc | Impact |
|---|---|---|---|
| `uint64_t` alignment | 8 bytes | 4 bytes | All fields after it shift |
| `double` alignment | 8 bytes | 4 bytes | All fields after it shift |
| `enum` size | always 4 bytes | 1 byte with `-fshort-enums` | Size + offset shift |
| Nested struct with above | Compounds the mismatch | | Cascading offset errors |

A struct like this will silently corrupt data on x86-32:

```c
struct device_info {
    uint8_t type;       // offset 0 on both
    uint64_t serial;    // offset 8 (wasm32) vs 4 (x86-32 gcc) — MISMATCH
};
```

The native host reads `serial` at offset 4, but the WASM app wrote it at
offset 8. The host sees garbage.

## The Solution

### Compile-time: `_Static_assert` pattern

The safest approach is a shared header compiled by both wasm32-clang and the
native compiler, with `_Static_assert` on every field offset:

```c
struct sensor_report {
    uint8_t sensor_id;
    struct sensor_reading reading;
    uint64_t timestamp __attribute__((aligned(8)));  // force 8-byte alignment
    double precision __attribute__((aligned(8)));
    uint8_t status;
} __attribute__((aligned(8)));

_Static_assert(offsetof(struct sensor_report, timestamp) == 16, "");
_Static_assert(sizeof(struct sensor_report) == 48, "");
```

Rules for portable struct layout:
1. Add `__attribute__((aligned(8)))` on `uint64_t` and `double` members
2. Replace `enum` with `uint32_t` + `#define` constants
3. Add `__attribute__((aligned(8)))` on the struct itself
4. Check nested structs — an inner struct with mismatched layout cascades
   to all outer fields after it

### Build-time: Automatic struct layout checker

This sample includes a Python tool (`check_struct_layout.py`) that
**automatically** detects layout mismatches without manual `_Static_assert`.

#### How it works

**Step 1: Parse NativeSymbol arrays**

WAMR native APIs are registered via `NativeSymbol` arrays in C source:

```c
static NativeSymbol native_symbols[] = {
    { "process_report", process_report_native, "(*~)i", NULL },
    { "configure_device", configure_device_native, "(*~)i", NULL },
    { "process_raw", process_raw_native, "(*~)i", NULL },
};
```

The tool uses regex to find these entries and extract the function name and
signature string from each.

**Step 2: Extract struct types from pointer parameters**

The WAMR signature string tells which parameters are pointers. In `"(*~)i"`:
- `*` = pointer (first parameter after `wasm_exec_env_t`)
- `~` = size of preceding buffer
- `i` = int32 return

The tool finds the C function definition for each function name and matches
the `*` positions to the actual parameter declarations:

```c
static int
process_report_native(wasm_exec_env_t exec_env,     // always skipped
                      struct sensor_report *rpt,     // * → struct sensor_report
                      int size)                      // ~
```

If the parameter is `struct <name> *`, the struct name is recorded for
checking. If it's `void *`, `char *`, `uint8_t *`, or any non-struct pointer,
the tool **cannot verify layout** — the cast target is only known at runtime
inside the function body. These are reported as "unchecked pointers" so the
developer knows to verify manually.

**Step 3: Locate struct-defining headers**

The tool follows `#include "..."` directives in the source file to find
which header defines each struct (searches for `struct <name> {`). It also
searches sibling directories of the source file — so if your source is in
`src/` and structs are in `shared/`, it finds them automatically.

**Step 4: Generate and compile a probe**

The tool generates a tiny C file that includes the struct headers and declares
one global per struct:

```c
#include <stdint.h>
#include <stddef.h>
#include "/absolute/path/to/struct_consistent.h"
#include "/absolute/path/to/struct_inconsistent.h"

struct sensor_report __attribute__((used)) __probe_sensor_report;
struct device_report __attribute__((used)) __probe_device_report;
```

This same file is compiled **twice** with `-g` (debug info):
- `gcc -m32 -g -c probe.c -o probe_native.o` — native layout
- `wasi-sdk/bin/clang --target=wasm32 -g -c probe.c -o probe_wasm.o` — wasm32 layout

The probe is compiled with `-c` only (no linking), so unknown symbols don't
matter — the probe only includes struct-defining headers, not the native API
implementation. `__attribute__((used))` prevents the compiler from optimizing
out the globals so the struct definitions appear in DWARF debug info.

The `.o` files are never linked or executed — the tool only reads the debug
metadata and discards them in a temp directory.

**Step 5: Extract DWARF debug info**

Both compilers embed DWARF debug info in the `.o` files, containing the exact
layout of every struct: field names, offsets (`DW_AT_data_member_location`),
sizes (`DW_AT_byte_size`), and type names.

- **Native `.o`** is ELF format — parsed with `pyelftools` (pure Python)
- **WASM `.o`** embeds DWARF as custom sections that `pyelftools` can't read
  — parsed with `llvm-dwarfdump` from wasi-sdk (handles WASM natively)

For type resolution, DWARF forms reference chains:
`DW_TAG_member` → `DW_AT_type(uint64_t)` → `DW_TAG_typedef` → `DW_TAG_base_type(unsigned long long, sz=8)`.
The tool follows these chains to find the actual byte size and detects when
the resolved type is `DW_TAG_structure_type` (a nested struct).

**Step 6: Compare and report**

Each struct's fields are compared between native and wasm32: offset, size,
and total struct size. Mismatches produce per-field fix suggestions based on
the type of mismatch:

- **8-byte type at wrong offset** → alignment issue →
  "add `__attribute__((aligned(8)))`"
- **Different size (4 vs 1 byte)** → enum with `-fshort-enums` →
  "replace with `uint32_t`"
- **Smaller type at wrong offset** → cascading from earlier mismatch →
  "caused by earlier field alignment mismatch"
- **Nested struct size differs** → inner struct has its own mismatch →
  "fix the inner struct's alignment first"

**Step 7: Recursive nested struct checking**

When a member's type is another struct, the tool adds that inner struct name
to a check queue. After checking the outer struct, it processes the queue —
discovering and checking inner structs without any manual configuration.
This catches cases where the inner struct itself is fine in isolation but
causes offset shifts in the outer struct due to different total size.

The tool produces per-field fix suggestions:

```
=== struct device_info ===
  serial    off=4  sz=8    off=8  sz=8    MISMATCH (offset)

  Suggested fixes for struct device_info:
    - 'serial' (uint64_t): alignment differs — add __attribute__((aligned(8)))
    - Add __attribute__((aligned(8))) to the struct itself
```

### CMake integration

The checker integrates into CMake as a configure-time check. For this sample:

```cmake
include(cmake/check_struct_layout.cmake)
check_wasm_struct_layout(
  SOURCE       ${CMAKE_CURRENT_SOURCE_DIR}/src/native_impl.c
  NATIVE_CC    ${CMAKE_C_COMPILER}
  NATIVE_FLAGS "-m32"
  WASI_SDK     /opt/wasi-sdk
)
```

On mismatch, CMake emits a WARNING with specific fix suggestions. On success,
a STATUS message confirms PASS. The build continues either way.

For WAMR itself, the checker is available as an opt-in build option:

```bash
cmake .. -DWAMR_CHECK_STRUCT_LAYOUT=ON
```

This scans all enabled WAMR native libraries at configure time. Each native
library's cmake file self-registers its NativeSymbol source via
`list(APPEND WAMR_NATIVE_API_SOURCES ...)`.

## This Sample

The sample demonstrates both the problem and the solution with two nested structs:

**Consistent** (`shared/struct_consistent.h`):
```c
struct sensor_reading { uint32_t raw_value; float calibrated; };
struct sensor_report {
    uint8_t sensor_id;
    struct sensor_reading reading;
    uint64_t timestamp __attribute__((aligned(8)));
    double precision __attribute__((aligned(8)));
    uint8_t status;
} __attribute__((aligned(8)));
```

**Inconsistent** (`shared/struct_inconsistent.h`):
```c
struct device_info { uint8_t type; uint64_t serial; };  // no alignment attr
struct device_report {
    uint8_t id;
    struct device_info info;                             // inner struct misaligned
    float voltage;
    double calibration;                                  // no alignment attr
    uint8_t mode;
};
```

The WASM app fills both structs and passes them to native APIs. The native
host prints Expected vs Host-read values for every field, showing:
- Consistent struct: all fields match
- Inconsistent struct: fields after `device_info` read garbage

A `void*` native function (`process_raw`) is included to demonstrate the
unchecked pointer warning.

## Requirements

- Native C compiler (gcc or clang)
- [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)
- Python 3.6+ with `pyelftools` (for the layout checker)

## Build and Run

```bash
# Build host app
mkdir cmake_build && cd cmake_build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
cd ..

# Build WASM app
mkdir -p out/wasm-app
/opt/wasi-sdk/bin/clang --target=wasm32 -O2 -Ishared \
    -z stack-size=4096 -Wl,--initial-memory=65536 \
    -Wl,--export=run -Wl,--export=__heap_base,--export=__data_end \
    -Wl,--no-entry -Wl,--allow-undefined -nostdlib \
    -o out/wasm-app/main.wasm wasm-app/main.c

# Run
./cmake_build/struct_check -f out/wasm-app/main.wasm
```

Or use the build script:

```bash
./build.sh
```

### Expected output

```
=== process_report (consistent nested struct) ===
  sizeof: WASM=48  native=48

  Field                 Expected            Host read           Match
  ────────────────────  ─────────────────────────────────────────────
  sensor_id             0x42                0x42                OK
  reading.raw_value     1024                1024                OK
  reading.calibrated    23.50               23.50               OK
  timestamp             0x1234567890ABCDEF  0x1234567890ABCDEF  OK
  flags                 255                 255                 OK
  precision             0.001000            0.001000            OK
  status                0x01                0x01                OK

  Result: 0 errors (PASS)

=== configure_device (inconsistent nested struct) ===
  sizeof: WASM=48  native=36  MISMATCH!

  Field                 Expected            Host read           Match
  ────────────────────  ─────────────────────────────────────────────
  id                    0x07                0x07                OK
  info.type             0x03                0x00                WRONG
  info.serial           0xDEADBEEFCAFEBABE  0x0000000000000003  WRONG
  voltage               3.29                0xCAFEBABE          WRONG
  channel               0x05                0xEF                WRONG
  calibration           1.234567            0x0000000540533333  WRONG
  mode                  0xAB                0x1B                WRONG

  Result: 7 errors (FAIL — layout mismatch causes wrong values)
```

### Run the checker standalone

```bash
pip install pyelftools

python3 check_struct_layout.py \
    --source src/native_impl.c \
    --native-cc gcc \
    --native-flags="-m32" \
    --wasi-sdk /opt/wasi-sdk \
    --verbose
```

### Checker arguments

| Argument | Required | Description |
|---|---|---|
| `--source` | Yes | C source file(s) with NativeSymbol arrays |
| `--native-cc` | Yes | Native C compiler path |
| `--wasi-sdk` | Yes | Path to wasi-sdk installation |
| `--include-dir` | No | Extra include dirs (auto-discovered from sibling dirs) |
| `--native-flags` | No | Extra native compiler flags (use `=` syntax) |
| `--wasm-flags` | No | Extra wasm compiler flags |
| `--structs` | No | Override auto-discovery (comma-separated names) |
| `--verbose` | No | Full field-by-field comparison table |
| `--quiet` | No | Only mismatches and summary |

### Exit codes

- `0` — all structs match
- `1` — at least one mismatch
- `2` — tool error

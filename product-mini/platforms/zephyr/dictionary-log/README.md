# WASM Dictionary-based Logging for Zephyr

Demonstrates how WASM apps on Zephyr can use dictionary-based logging to eliminate format strings from the WASM data segment, reducing binary size.

## How It Works

1. **Build time:** `extract_log_strings.py` preprocesses WASM app C sources (`clang -E`), then scans the preprocessed output for `wasm_log()` calls (all macros, PRI format specifiers, and header includes already resolved). It assigns integer IDs, computes type descriptors, and generates transformed `.i` files where log calls use `wasm_log_dict(level, string_id, type_desc, args)`. Supports multi-file WASM apps (multiple `.c` files compiled and linked together).

2. **Runtime:** The native `wasm_log_dict()` wrapper packs a compact binary packet (msg_type=0x80) with the string ID, timestamp, and typed argument values, then emits it via Zephyr's `LOG_HEXDUMP_*` macros through a dedicated `wasm_dict` log module. This automatically works with any Zephyr log backend (UART, RTT, network, BLE, filesystem) — no backend-specific code needed.

3. **Offline:** `decode_wasm_log.py` reads the captured output, identifies WASM dict packets embedded inside Zephyr native log messages (by checking the data field for our 0x80 marker), and decodes them using the generated dictionary.

## Prerequisites

- Zephyr SDK and `west` tool
- wasi-sdk (set `WASI_SDK_DIR` if not in `/opt/wasi-sdk*`)
- Python 3
- `colorama` Python package (for colored log output matching Zephyr's style):
  ```bash
  pip install colorama
  ```
  The decoder works without it (falls back to plain text), but colors make the output much easier to read: err=red, wrn=yellow, inf=green, dbg=blue.

## Build

```bash
# Default: full dictionary mode (all output binary, decode offline)
west build -b qemu_x86 .

# Development mode: native logs readable in terminal, WASM still binary
west build -b qemu_x86 . -- -DWAMR_ZEPHYR_DICT_LOG=OFF
```

Both modes:
- Preprocess C sources (`clang -E`) to resolve all macros and includes
- Extract log strings from preprocessed output, assign IDs, transform
- Compile both baseline (with format strings) and dictionary (without) WASM variants
- Embed both in the Zephyr ELF with the WAMR runtime
- Print size comparison

The sensor app demonstrates multi-file compilation (2 `.c` files + 1 `.h` header).

## Configuration

### Zephyr Dictionary Logging Toggle

By default, both native Zephyr logs and WASM logs use dictionary (binary) mode — all output is hex-encoded and requires offline decoding. To see native Zephyr logs as readable text in the terminal while keeping WASM dictionary logging active:

```bash
west build -b qemu_x86 . --pristine -- -DWAMR_ZEPHYR_DICT_LOG=OFF
```

| Mode | Native Logs | WASM Dict Logs | Decode Needed |
|------|------------|----------------|---------------|
| ON (default) | Binary hex (needs `--zephyr-db`) | Binary hex (embedded in native stream) | Full decode |
| OFF | Human-readable in terminal | Text hexdump (from `wasm_dict` module) | Only `--wasm-db` |

With dict OFF, WASM packets appear as `LOG_HEXDUMP` text output from the `wasm_dict` module. The decoder parses these text hexdumps automatically:

```bash
# Dict OFF: only WASM decode needed (native logs visible in raw output)
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    /tmp/serial.log --hex
```

This is useful during development — you see native Zephyr logs immediately in the terminal without needing to run the decoder, while WASM logs are still compressed for binary size savings.

## Run

```bash
west build -t run 2>&1 | tee /tmp/serial.log
# Press Ctrl+A then X to exit QEMU
```

The host app runs five groups of log messages:
1. **Native Zephyr logs** (`dict_log_demo` module) — binary hex (dict ON) or readable text (dict OFF)
2. **Baseline sensor app** — format strings in WASM data segment, formatted at runtime
3. **Dictionary sensor app** (app_id=0) — binary packets, decoded offline
4. **Baseline network app** — format strings in WASM data segment, formatted at runtime
5. **Dictionary network app** (app_id=1) — binary packets, decoded offline

## Decode

The decoder needs two things to show full output:
1. **WASM dictionary** (`--wasm-db`) — always required, decodes WASM 0x80 packets
2. **Zephyr dictionary** (`--zephyr-db`) + Zephyr parser scripts — needed for native log packets

The decoder auto-discovers Zephyr's parser scripts from `~/zephyrproject/zephyr/`. If your Zephyr is installed elsewhere, set `ZEPHYR_BASE` explicitly:

```bash
# Full decode (dict ON mode): native + WASM packets (already in timestamp order)
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex

# If Zephyr is installed elsewhere, set ZEPHYR_BASE:
ZEPHYR_BASE=/path/to/zephyr python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex

# Dict OFF mode: only WASM decode needed (native logs already in terminal)
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    /tmp/serial.log --hex

# Single app only (backward compatible, no app_id prefix = app_id 0):
python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_log_dict.json \
    /tmp/serial.log --hex
```

### Troubleshooting: Missing Native Logs

If you only see WASM dictionary logs and no native `dict_log_demo` messages:

1. **Built with `WAMR_ZEPHYR_DICT_LOG=OFF`**: Native logs are human-readable text in the raw serial output — they don't appear in decoder output because they were never binary-encoded. Check `/tmp/serial.log` directly.
2. **Missing `--zephyr-db`**: Without this flag, native packets are skipped entirely.
3. **Missing `colorama`**: The Zephyr parser requires `pip install colorama` — without it, the parser import fails silently and native packets are skipped.
4. **Zephyr parser not found**: Run with `-v` to see debug output — look for "ZEPHYR_BASE not set" or "Failed to import" messages. Fix by setting `ZEPHYR_BASE`.

### The `--sort` Flag

With the structured LOG_HEXDUMP approach, all packets (native + WASM) flow through Zephyr's unified log stream and are already in timestamp order. The `--sort` flag is no longer needed for basic ordering but remains available if you want to enforce strict timestamp sorting across all decoded lines.

### Expected Output

The output shows five groups of log messages in chronological order (native logs, baseline sensor, dict sensor, baseline network, dict network). With `colorama` installed, each log level has a matching color (err=red, wrn=yellow, inf=green, dbg=blue) — same as Zephyr's native `log_parser.py`.

```
*** Booting Zephyr OS build v4.4.0-rc2 ***
[        10] <inf> dict_log_demo: === Dictionary Logging Demo ===
[        10] <err> dict_log_demo: Native ERR: error code -5 on subsystem 3
[        10] <wrn> dict_log_demo: Native WRN: retry count 100 exceeds threshold 50
[        10] <inf> dict_log_demo: Native INF: sensor BME280 initialized, channels=3
...                                                    ← native Zephyr dictionary logs

[        50] <inf> wasm_app: My_APP=== WASM Sensor Monitor starting ===
[        50] <dbg> wasm_app: printf_out: My_APPInitializing 8 sensor channels
...                                                    ← baseline WASM (runtime formatted)
                                                         note: "My_APP" prefix, "printf_out:" artifacts

[       100] <inf> wasm_app: === WASM Sensor Monitor starting ===
[       100] <inf> wasm_app: Firmware version 2.5.1 built for qemu_x86
[       100] <dbg> wasm_app: Initializing 8 sensor channels
...                                                    ← dict sensor app (app_id=0, clean output)

[       190] <inf> network_app: === Network Stack starting ===
[       190] <inf> network_app: TCP listen port=8080, max connections=4
[       190] <err> network_app: Connection reset by peer: socket=3, error code=-104
...                                                    ← dict network app (app_id=1)

[       200] <inf> dict_log_demo: === WASM apps finished ===
[       200] <inf> dict_log_demo: --- Demo complete ---
```

The contrast between baseline (messy `My_APP` prefix, `printf_out:` function leak) and dictionary apps (clean message text, distinct app names) demonstrates both the quality improvement and multi-app capability.

## Size Comparison

The build prints sizes of all WASM variants:

```
--- Sensor App (2 .c files + 1 .h, multi-file) ---
Baseline:     13,260 bytes  (135 log calls, format strings in binary)
Dictionary:    5,869 bytes  (135 log calls, string IDs only) — 56% smaller

--- Network App (single .c file) ---
Baseline:      3,091 bytes  ( 28 log calls, format strings in binary)
Dictionary:    1,525 bytes  ( 28 log calls, string IDs only) — 51% smaller
```

The savings come from eliminating format string literals from the WASM data segment. Both apps show 50%+ reduction. The sensor app (multi-file) demonstrates that the preprocessor-based extraction handles multiple source files with shared headers seamlessly.

## Key Advantage

Unlike Zephyr's native dictionary logging (which embeds format strings in the ELF and requires recompilation when logs change), our approach keeps the Zephyr ELF unchanged. Only the WASM binary and its dictionary JSON need updating when log messages change. Different WASM apps with different log strings can run on the same firmware.

## Multi-App Support

Multiple WASM apps can run on the same firmware, each with its own dictionary. The host assigns an `app_id` (0-255) to each app via exec_env user_data — the WASM app cannot choose or forge its own ID.

```bash
# Decode with multiple app dictionaries:
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex
```

Output shows distinct app names derived from the JSON filename:
```
[       100] <inf> wasm_app: === WASM Sensor Monitor starting ===
[       190] <inf> network_app: === Network Stack starting ===
```

### Why Per-App Dictionaries (Not Unified)

We evaluated merging all apps into a single shared dictionary to deduplicate common format strings. Finding: **not worth it**.

- Only 4 shared strings out of 163 total (< 1% overlap)
- A unified dictionary would only save ~240 bytes of **JSON file size on the host PC** — it does NOT reduce WASM binary size at all, since the WASM binary only contains integer IDs regardless of how the JSON is organized
- Cost: apps can no longer be built/deployed independently, ID offset coordination required, more complex decoder setup

The per-app approach is correct: each app gets its own dictionary, can be updated independently, and the WASM binary size (the thing that matters on the embedded device) is unaffected by dictionary organization.

## Limitations

### Max 8 Arguments Per Log Call

The type descriptor is a uint32 with 4 bits per argument, limiting each log call to 8 typed arguments maximum. Calls exceeding this are skipped with a warning at build time.

### String Argument Length Limit

String arguments (`%s`) are packed into the binary packet as length-prefixed data: `[type=0x04][len:2B LE][string bytes]`. The total packet size is capped at 256 bytes (`WASM_LOG_DICT_MAX_PACKET`). After the 14-byte header and other args, this leaves roughly 200 bytes for string content. Strings exceeding the remaining space are truncated at runtime (no build-time warning — the string value isn't known until runtime).

### What IS Supported (Not Limitations)

The following are fully handled by the preprocessor-based extraction:

- **PRI format macros** (`PRIu32`, `PRId64`, etc.) — resolved by `clang -E`
- **LOG calls in header files** (inline functions, shared helpers) — inlined by preprocessor
- **Multi-file WASM apps** (multiple `.c` files linked together) — flat ID space across all files
- **String concatenation** (`"part1" "part2"`) — merged by preprocessor
- **Macro-wrapped LOG calls** — expanded before extraction
- **`wasm_log` text inside format strings** — e.g., `LOG_ERR("calling wasm_log(%d) failed", err)`
  is handled correctly (the paren matcher tracks string literal boundaries)

## Design: Why Regex on Preprocessed Output (Not AST)

The extraction tool uses regex pattern matching on `clang -E` preprocessed output rather than C AST parsing (e.g., libclang). This is a deliberate choice:

**Why regex works perfectly here:**

After `clang -E`, the preprocessor has already:
- Resolved all macros (PRI, LOG_AT, etc.)
- Expanded all includes (headers inlined)
- Merged string concatenation
- Stripped comments

The resulting `.i` file contains `wasm_log(level, "literal_string", args)` — a simple, unambiguous pattern. The format string is guaranteed to be a literal (the preprocessor ensures this). Our paren-matching parser correctly handles `wasm_log` text appearing inside format strings by tracking `in_string` state.

**Why AST adds no benefit:**

| Concern | Regex on `.i` | AST (libclang) |
|---------|---------------|----------------|
| Correctness | Preprocessor resolves ambiguity | Same (also needs preprocessing first) |
| False positives | None after `clang -E` strips comments | None |
| Security | clang validates before AND after | Same |
| Type inference | From `%d`/`%s`/`%f` specifiers | Same (format string is runtime, not typed in AST) |
| Dependencies | Python stdlib only | Requires `libclang` or JSON AST parsing |
| Speed | Fast text scan | Slower (full parse tree) |
| Maintenance | Simple, version-independent | AST format is clang-version-dependent |

**The trust model makes AST unnecessary:**

```
Source → [clang -fsyntax-only] → [clang -E] → [our regex] → [clang compile] → [WASM sandbox]
          validates input          resolves       transforms    validates output   validates runtime
```

Our regex is sandwiched between two clang validation passes. It cannot produce dangerous output — only wrong output (which clang's second pass rejects). AST would add complexity without improving correctness or security.

## Binary Packet Format (msg_type=0x80)

```
Offset  Size   Field
0       1B     msg_type = 0x80 (vendor extension range)
1       1B     app_id (0-255, host-assigned, not controlled by WASM app)
2       1B     log_level (1=ERR, 2=WRN, 3=INF, 4=DBG, 5=VERBOSE)
3       2B     string_id (uint16 LE)
5       8B     timestamp (uint64 LE)
13      1B     arg_count
14+     N      arg entries: [type:1B][value:4-8B]
```

Arg types: 0x01=int32(4B), 0x02=int64(8B), 0x03=float64(8B), 0x04=string(2B len + data)

## Security Analysis

The dictionary logging transformation does NOT weaken the WASM sandbox. A malicious WASM app controls `log_level`, `string_id`, `arg_type_descriptor`, and `va_args` — but none of these provide an escape vector:

| Attack Vector | Why It Fails |
|---------------|-------------|
| Read host memory via string offset | `validate_app_str_addr()` bounds-checks within WASM linear memory |
| Read past va_args bounds | `wasm_runtime_get_native_addr_range()` enforces limits |
| Buffer overflow in packet | Every arg write checked against `WASM_LOG_DICT_MAX_PACKET` (256B), emitted via kernel LOG_HEXDUMP |
| Forge app_id | Host-assigned via exec_env user_data — WASM has no API to modify it |
| Malicious string_id/type_desc | Only affects offline decoder (Python on host), not runtime memory |

The worst case: a malicious WASM app emits garbage packets that decode to nonsense — a denial-of-observability, not a sandbox escape. This is equivalent to the baseline `wasm_log()` path where a malicious app could print misleading text.

## Tests

```bash
cd product-mini/platforms/zephyr/dictionary-log
python3 -m pytest tests/ -v
```

Three test suites (90 tests total):

- **`test_multifile_extraction.py`** (54 tests) — invokes the extraction script
  as a subprocess on C fixture files:
  - **Multi-file** (3 `.c` + 2 `.h`): flat ID space, header inline LOG extracted,
    PRI macros resolved, shared headers get separate IDs per call site,
    duplicate strings across files get different IDs, file order affects IDs
  - **Format type classification** (`types_test.c`): `%d`/`%i`/`%u`/`%x`/`%X`/`%o`
    → int32, `%c`/`%p` → int32, `%ld`/`%llu`/`%llx` → int64,
    `%f`/`%e`/`%g`/`%F`/`%E`/`%G` → float64, `%s` → string,
    `%%` not counted, width/precision modifiers, PRI macros, mixed types,
    zero args, max 8 args, escaped quotes, backslash sequences, long strings,
    adjacent calls on same line
  - **Edge cases** (`edge_cases.c`): `#if 0` block excluded, variable format
    string skipped, valid call after skip, comments before LOG, empty functions,
    `wasm_log` text inside format strings, `wasm_log` in other function args,
    function pointer usage, struct field named `wasm_log`, C++ file rejected
  - **Error handling**: syntax errors caught, missing includes caught,
    empty files, no-log-call files
  - **Single file**: basic sanity, output file naming

- **`test_decode_truncation.py`** (21 tests) — tests the offline decoder core:
  - Valid packet decoding (all arg types: int32, int64, float64, string)
  - Truncated packets at every level (header, int32, int64, float64, string
    length, string data)
  - Unknown string_id, unknown app_id, unknown arg_type
  - Offset handling and sequential packet decoding

- **`test_structured_output.py`** (15 tests) — tests the LOG_HEXDUMP structured
  output layer:
  - Embedded WASM packet extraction from native msg data field
  - Native msgs without data or with non-WASM data correctly skipped
  - Mixed stream (native + embedded WASM) decoded correctly
  - Legacy standalone 0x80 packet backward compatibility
  - Text hexdump parsing (dict OFF mode): basic, multiline, multi-block,
    other modules ignored, non-0x80 data ignored, all log levels
  - Unified stream extraction (all hex after separator, pre-separator ignored)

Test fixtures:
```
tests/
├── multifile/       3 .c + 2 .h (multi-file compilation scenarios)
├── singlefile/      types_test.c + edge_cases.c (single-file type/edge tests)
└── error_cases/     syntax_error.c, missing_include.c, no_log_calls.c, empty_file.c, rejected.cpp
```

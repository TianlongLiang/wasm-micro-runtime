# WASM Dictionary-based Logging for Zephyr

Demonstrates how WASM apps on Zephyr can use dictionary-based logging to eliminate format strings from the WASM data segment, reducing binary size.

## How It Works

1. **Build time:** `extract_log_strings.py` preprocesses WASM app C sources (`clang -E`), then scans the preprocessed output for `wasm_log()` calls (all macros, PRI format specifiers, and header includes already resolved). It assigns integer IDs, computes type descriptors, and generates transformed `.i` files where log calls use `wasm_log_dict(level, string_id, type_desc, args)`. Supports multi-file WASM apps (multiple `.c` files compiled and linked together).

2. **Runtime:** The native `wasm_log_dict()` wrapper packs a compact 5-byte header + typed arguments into a binary packet and emits it via Zephyr's `LOG_HEXDUMP_*` macros through a dedicated `wasm_dict` log module. No timestamp embedded — Zephyr's log subsystem provides the timestamp in the native packet header. This automatically works with any Zephyr log backend (UART, RTT, network, BLE, filesystem) — no backend-specific code needed.

3. **Stitch:** `stitch_wasm_dicts.py` merges per-app dictionary JSONs into a single unified file, so the decoder only needs one `--wasm-db` argument regardless of how many WASM apps are running.

4. **Offline:** `decode_wasm_log.py` reads the captured output, identifies WASM dict packets by checking the Zephyr `source` field (module name `wasm_dict`), and decodes them using the unified dictionary.

## Prerequisites

- Zephyr SDK and `west` tool
- wasi-sdk (set `WASI_SDK_DIR` if not in `/opt/wasi-sdk*`)
- Python 3
- `colorama` Python package (for colored output):
  ```bash
  pip install colorama
  ```
  The decoder works without it (falls back to plain text). In dict ON (binary) mode, colors match Zephyr's `log_parser.py` scheme: ERR=red, WRN=yellow, INF=green, DBG=blue. In dict OFF (text) mode, colors match Zephyr's text backend: ERR=bright red, WRN=bright yellow, INF/DBG=white. Use `--auto-color` in dict OFF mode to detect and match custom Zephyr color configurations (ignored in dict ON mode).

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
    --wasm-db build/wasm_unified_dict.json \
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

## Stitch (Multi-App)

When running multiple WASM apps, first merge their per-app dictionaries into a single unified file:

```bash
python3 scripts/stitch_wasm_dicts.py \
    --app 0:sensor_app:build/wasm_log_dict.json \
    --app 1:network_app:build/wasm_log_dict_network.json \
    -o build/wasm_unified_dict.json
```

Each `--app` argument: `app_id:app_name:path_to_json`. The output is a JSON array sorted by app_id.

## Decode

The decoder needs two things to show full output:
1. **Unified WASM dictionary** (`--wasm-db`) — always required, decodes WASM packets
2. **Zephyr dictionary** (`--zephyr-db`) + Zephyr parser scripts — required in dict ON mode to identify WASM packets by source_id and decode native logs

The decoder identifies WASM packets by looking up the `source` field in the Zephyr native packet header — if it maps to the `wasm_dict` module, the data payload is decoded as a WASM packet. This means `--zephyr-db` is required in dict ON (binary) mode.

The decoder auto-discovers Zephyr's parser scripts from `~/zephyrproject/zephyr/`. If your Zephyr is installed elsewhere, set `ZEPHYR_BASE` explicitly:

```bash
# Full decode (dict ON mode): native + WASM packets (already in timestamp order)
python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_unified_dict.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex

# If Zephyr is installed elsewhere, set ZEPHYR_BASE:
ZEPHYR_BASE=/path/to/zephyr python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_unified_dict.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex

# Dict OFF mode: only WASM decode needed (native logs already in terminal)
python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_unified_dict.json \
    /tmp/serial.log --hex

# Auto-detect colors (use if Zephyr color config differs from default):
python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_unified_dict.json \
    /tmp/serial.log --hex --auto-color
```

### Troubleshooting: Missing Native Logs

If you only see WASM dictionary logs and no native `dict_log_demo` messages:

1. **Built with `WAMR_ZEPHYR_DICT_LOG=OFF`**: Native logs are human-readable text in the raw serial output — they don't appear in decoder output because they were never binary-encoded. Check `/tmp/serial.log` directly.
2. **Missing `--zephyr-db`**: In dict ON mode, `--zephyr-db` is required both to identify WASM packets (via source_id lookup) and to decode native packets. Without it, the decoder cannot function in binary mode.
3. **Missing `colorama`**: The Zephyr parser requires `pip install colorama` — without it, the parser import fails silently and native packets are skipped.
4. **Zephyr parser not found**: Run with `-v` to see debug output — look for "ZEPHYR_BASE not set" or "Failed to import" messages. Fix by setting `ZEPHYR_BASE`.

### The `--sort` Flag

With the structured LOG_HEXDUMP approach, all packets (native + WASM) flow through Zephyr's unified log stream and are already in timestamp order. The `--sort` flag is no longer needed for basic ordering but remains available if you want to enforce strict timestamp sorting across all decoded lines.

### Timestamp Format

The decoder uses the timestamp from Zephyr's native packet header (no timestamp embedded in the WASM packet itself). It auto-detects the display format from Zephyr's `log_parser.py` output and renders WASM packet timestamps to match:

| Format | Example | When Used |
|--------|---------|-----------|
| Zephyr uptime | `[00:00:12.345,000]` | Default on QEMU/most boards |
| RTC wall-clock | `[2026-05-13 02:36:13.218]` | Boards with RTC configured (falls back to uptime rendering) |
| Raw integer | `[     12345]` | Minimal timestamp configurations |

In dict OFF (text) mode, the timestamp is already present in the text output line — no formatting needed on the decoder side.

If the log file has no recognizable native log lines, the decoder defaults to Zephyr uptime format.

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

The workflow for multi-app:
1. Extract each app's dictionary independently (each gets its own JSON)
2. Stitch them into a unified JSON with `stitch_wasm_dicts.py`
3. Pass the unified JSON to the decoder

```bash
# 1. Extract per-app dictionaries (done during build)
python3 scripts/extract_log_strings.py ... -j build/sensor_dict.json sensor_app.c
python3 scripts/extract_log_strings.py ... -j build/network_dict.json network_app.c

# 2. Stitch into unified JSON
python3 scripts/stitch_wasm_dicts.py \
    --app 0:sensor_app:build/sensor_dict.json \
    --app 1:network_app:build/network_dict.json \
    -o build/wasm_unified_dict.json

# 3. Decode
python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_unified_dict.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex
```

Output shows distinct app names from the unified dictionary:
```
[       100] <inf> sensor_app: === WASM Sensor Monitor starting ===
[       190] <inf> network_app: === Network Stack starting ===
```

### Unified Dictionary Format

The stitch tool outputs a JSON array:
```json
[
  {"app_id": 0, "app_name": "sensor_app", "dict": {"0": {"fmt": "...", "arg_types": [...]}, ...}},
  {"app_id": 1, "app_name": "network_app", "dict": {"0": {"fmt": "...", "arg_types": [...]}, ...}}
]
```

Each app is built and extracted independently — the stitch step just packages them together for the decoder. WASM binary size is unaffected by dictionary organization (it contains only integer IDs).

## Limitations

### Max 8 Arguments Per Log Call

The type descriptor is a uint32 with 4 bits per argument, limiting each log call to 8 typed arguments maximum. The extraction script errors and aborts if a call exceeds this limit — the developer must reduce arguments or split into multiple log calls.

### String Argument Length Limit

String arguments (`%s`) are packed into the binary packet as length-prefixed data: `[type=0x04][len:2B LE][string bytes]`. The total packet size is capped at 256 bytes (`WASM_LOG_DICT_MAX_PACKET`). After the 5-byte header and other args, this leaves roughly 250 bytes for string content. For calls without `%s`, the extraction script validates the packet size at build time and errors if it exceeds 256 bytes. For calls with `%s`, the C emitter truncates at runtime since string length isn't known until then.

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

## Binary Packet Format (V2)

The WASM dict packet is emitted as the data payload of a Zephyr `LOG_HEXDUMP_*` call through the `wasm_dict` module. The decoder identifies it by checking the `source` field in the enclosing Zephyr native packet header (maps to module name `wasm_dict` in `log_dictionary.json`).

```
Offset  Size   Field
0       1B     app_id (uint8, 0-255, host-assigned via exec_env user_data)
1       1B     log_level (uint8: 1=ERR, 2=WRN, 3=INF, 4=DBG, 5=VERBOSE)
2       2B     log_string_id (uint16 LE, index into per-app dictionary)
4       1B     arg_count (uint8, 0-8)
5+      N      typed arg entries: [type:1B][value]
```

Arg types:

| Type Code | Name | Value Size | Encoding |
|-----------|------|-----------|----------|
| 0x01 | int32 | 4B | signed int32 LE |
| 0x02 | int64 | 8B | signed int64 LE |
| 0x03 | float64 | 8B | IEEE 754 double LE |
| 0x04 | string | 2B + N | uint16 LE length + UTF-8 bytes |

All multi-byte integers are little-endian. Maximum packet size: 256 bytes. Timestamp is NOT embedded — it comes from the enclosing Zephyr native packet header (4B `k_uptime_get_32()` at offset 10).

Example packet for `wasm_log_dict(INF, 5, 0x41, ptr, name)` where ptr=0x2000, name="eth0":
```
[00]                         app_id = 0
[03]                         log_level = INF
[05 00]                      log_string_id = 5
[02]                         arg_count = 2
[01][00 20 00 00]            int32: 0x2000
[04][04 00][65 74 68 30]     string: len=4, "eth0"
```
Total: 17 bytes.

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

Four test suites (146 tests total):

- **`test_multifile_extraction.py`** (56 tests) — invokes the extraction script as a subprocess on C fixture files:
  - **Multi-file** (3 `.c` + 2 `.h`): flat ID space, header inline LOG extracted, PRI macros resolved, shared headers get separate IDs per call site, duplicate strings across files get different IDs, file order affects IDs
  - **Format type classification** (`types_test.c`): `%d`/`%i`/`%u`/`%x`/`%X`/`%o` → int32, `%c`/`%p` → int32, `%ld`/`%llu`/`%llx` → int64, `%f`/`%e`/`%g`/`%F`/`%E`/`%G` → float64, `%s` → string, `%%` not counted, width/precision modifiers, PRI macros, mixed types, zero args, max 8 args, escaped quotes, backslash sequences, long strings, adjacent calls on same line
  - **Edge cases** (`edge_cases.c`): `#if 0` block excluded, variable format string skipped, valid call after skip, comments before LOG, empty functions, `wasm_log` text inside format strings, `wasm_log` in other function args, function pointer usage, struct field named `wasm_log`, C++ file rejected
  - **Arg limit errors**: >8 args causes error and abort (not silent skip), 8 args passes
  - **Error handling**: syntax errors caught, missing includes caught, empty files, no-log-call files
  - **Single file**: basic sanity, output file naming

- **`test_decode_truncation.py`** (34 tests) — tests the offline decoder core (V2 packet format):
  - Valid packet decoding (all arg types: int32, int64, float64, string)
  - V2 format: 5-byte header (no 0x80 marker, no timestamp), message-only return
  - Truncated packets at every level (header, int32, int64, float64, string length, string data)
  - Unknown string_id, unknown app_id, unknown arg_type
  - Offset handling and sequential packet decoding
  - Pointer decode: `%p` as hex, NULL, double pointer, function pointer, array of pointers, mixed `%p`+`%d`+`%s`

- **`test_stitch_tool.py`** (16 tests) — tests the stitch tool and unified JSON loading:
  - Valid stitching: merge 2+ apps, single app, sorted output
  - Error handling: duplicate app_id, missing file, invalid JSON, bad format, non-integer ID
  - Unified JSON loading: parse_wasm_dbs loads correct mapping, routes by app_id, missing app_id returns None, empty JSON
  - End-to-end integration: stitch → decode roundtrip, pointer format, multi-type args

- **`test_structured_output.py`** (40 tests) — tests the LOG_HEXDUMP structured output layer:
  - Source-ID identification: wasm_dict source recognized, non-wasm source rejected, empty data rejected, unknown source rejected
  - Text hexdump parsing (dict OFF mode): basic, multiline, multi-block, other modules ignored, all log levels
  - Unified stream extraction (all hex after separator, pre-separator ignored)
  - Auto-color detection: detects per-level ANSI codes from native logs, custom colors, plain text fallback
  - Timestamp format auto-detection: uptime, RTC (falls back to uptime), raw integer, default fallback
  - Dict OFF text mode merge: native pass-through, hexdump decoded in-place, baseline `wasm_dict:` lines pass through, mixed output

Test fixtures:
```
tests/
├── multifile/       3 .c + 2 .h (multi-file compilation scenarios)
├── singlefile/      types_test.c + edge_cases.c (single-file type/edge tests)
└── error_cases/     syntax_error.c, missing_include.c, no_log_calls.c, empty_file.c, rejected.cpp, too_many_args.c
```

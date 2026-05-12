# WASM Dictionary-based Logging for Zephyr

Demonstrates how WASM apps on Zephyr can use dictionary-based logging to
eliminate format strings from the WASM data segment, reducing binary size.

## How It Works

1. **Build time:** `extract_log_strings.py` scans the WASM app source, extracts
   all `LOG_*` format strings, assigns integer IDs, and generates a transformed
   source where log calls use `wasm_log_dict(level, string_id, type_desc, args)`
   instead of embedding the format string.

2. **Runtime:** The native `wasm_log_dict()` wrapper packs a compact binary
   packet (msg_type=0x80) with the string ID, timestamp, and typed argument
   values, then emits it over UART in hex-encoded format alongside Zephyr's
   native dictionary log stream.

3. **Offline:** `decode_wasm_log.py` reads the UART output, dispatches native
   Zephyr packets to Zephyr's own parser, and decodes WASM packets using the
   generated `wasm_log_dict.json` dictionary.

## Prerequisites

- Zephyr SDK and `west` tool
- wasi-sdk (set `WASI_SDK_DIR` if not in `/opt/wasi-sdk*`)
- Python 3
- `colorama` Python package (for colored log output matching Zephyr's style):
  ```bash
  pip install colorama
  ```
  The decoder works without it (falls back to plain text), but colors make
  the output much easier to read: err=red, wrn=yellow, inf=green, dbg=blue.

## Build

```bash
# Default: full dictionary mode (all output binary, decode offline)
west build -b qemu_x86 .

# Development mode: native logs readable in terminal, WASM still binary
west build -b qemu_x86 . -- -DWAMR_ZEPHYR_DICT_LOG=OFF
```

Both modes:
- Extract log strings from the WASM app source
- Compile both baseline (with format strings) and dictionary (without) WASM variants
- Embed both in the Zephyr ELF with the WAMR runtime
- Print size comparison

## Configuration

### Zephyr Dictionary Logging Toggle

By default, both native Zephyr logs and WASM logs use dictionary (binary)
mode — all output is hex-encoded and requires offline decoding.

To see native Zephyr logs as readable text in the terminal while keeping
WASM dictionary logging active:

```bash
west build -b qemu_x86 . --pristine -- -DWAMR_ZEPHYR_DICT_LOG=OFF
```

| Mode | Native Logs | WASM Dict Logs | Decode Needed |
|------|------------|----------------|---------------|
| ON (default) | Binary hex (needs `--zephyr-db`) | Binary hex | Full decode |
| OFF | Human-readable in terminal | Binary hex (unchanged) | Only `--wasm-db` |

With dict OFF, the decoder automatically detects the missing `##ZLOGV1##`
separator and scans for WASM hex data directly:

```bash
# Dict OFF: only WASM decode needed (native logs visible in raw output)
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    /tmp/serial.log --hex --sort
```

This is useful during development — you see native Zephyr logs immediately
in the terminal without needing to run the decoder, while WASM logs are
still compressed for binary size savings.

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

The decoder auto-discovers Zephyr's parser scripts from `~/zephyrproject/zephyr/`.
If your Zephyr is installed elsewhere, set `ZEPHYR_BASE` explicitly:

```bash
# Full decode (dict ON mode): native + WASM packets
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex --sort

# If Zephyr is installed elsewhere, set ZEPHYR_BASE:
ZEPHYR_BASE=/path/to/zephyr python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex --sort

# Dict OFF mode: only WASM decode needed (native logs already in terminal)
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    /tmp/serial.log --hex --sort

# Single app only (backward compatible, no app_id prefix = app_id 0):
python3 scripts/decode_wasm_log.py \
    --wasm-db build/wasm_log_dict.json \
    /tmp/serial.log --hex
```

### Troubleshooting: Missing Native Logs

If you only see WASM dictionary logs and no native `dict_log_demo` messages:

1. **Built with `WAMR_ZEPHYR_DICT_LOG=OFF`**: Native logs are human-readable text
   in the raw serial output — they don't appear in decoder output because they
   were never binary-encoded. Check `/tmp/serial.log` directly.
2. **Missing `--zephyr-db`**: Without this flag, native packets are skipped entirely
3. **Missing `colorama`**: The Zephyr parser requires `pip install colorama` — without it,
   the parser import fails silently and native packets are skipped
4. **Zephyr parser not found**: Run with `-v` to see debug output — look for
   "ZEPHYR_BASE not set" or "Failed to import" messages. Fix by setting `ZEPHYR_BASE`

### The `--sort` Flag

WASM dictionary packets are emitted via direct `uart_poll_out` before Zephyr's
log backend prints its `##ZLOGV1##` separator. Without `--sort`, WASM packets
appear first regardless of actual timestamp. With `--sort`, all decoded lines
are sorted by timestamp into chronological order:

```
[10]  native Zephyr logs first
[50]  baseline WASM logs (runtime formatted)
[100] dictionary WASM logs (binary packets)
[180] native Zephyr post-summary
```

### Expected Output (with --sort)

The output shows three distinct groups of log messages in chronological order.
With `colorama` installed, each log level has a matching color (err=red,
wrn=yellow, inf=green, dbg=blue) — same as Zephyr's native `log_parser.py`.

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

The contrast between baseline (messy `My_APP` prefix, `printf_out:` function
leak) and dictionary apps (clean message text, distinct app names) demonstrates
both the quality improvement and multi-app capability.

## Size Comparison

The build prints sizes of all WASM variants:

```
--- Sensor App ---
Baseline:     13,288 bytes  (135 log calls, format strings in binary)
Dictionary:    5,721 bytes  (135 log calls, string IDs only) — 57% smaller

--- Network App ---
Baseline:      3,119 bytes  ( 28 log calls, format strings in binary)
Dictionary:    1,525 bytes  ( 28 log calls, string IDs only) — 51% smaller
```

The savings come from eliminating format string literals from the WASM data
segment. Both apps show 50%+ reduction. The sensor app eliminates ~7KB of
strings; the network app eliminates ~1.5KB.

## Key Advantage

Unlike Zephyr's native dictionary logging (which embeds format strings in the
ELF and requires recompilation when logs change), our approach keeps the Zephyr
ELF unchanged. Only the WASM binary and its dictionary JSON need updating when
log messages change. Different WASM apps with different log strings can run on
the same firmware.

## Multi-App Support

Multiple WASM apps can run on the same firmware, each with its own dictionary.
The host assigns an `app_id` (0-255) to each app via exec_env user_data —
the WASM app cannot choose or forge its own ID.

```bash
# Decode with multiple app dictionaries:
python3 scripts/decode_wasm_log.py \
    --wasm-db 0:build/wasm_log_dict.json \
    --wasm-db 1:build/wasm_log_dict_network.json \
    --zephyr-db build/zephyr/log_dictionary.json \
    /tmp/serial.log --hex --sort
```

Output shows distinct app names derived from the JSON filename:
```
[       100] <inf> wasm_app: === WASM Sensor Monitor starting ===
[       190] <inf> network_app: === Network Stack starting ===
```

### Why Per-App Dictionaries (Not Unified)

We evaluated merging all apps into a single shared dictionary to deduplicate
common format strings. Finding: **not worth it**.

- Only 4 shared strings out of 163 total (< 1% overlap)
- A unified dictionary would only save ~240 bytes of **JSON file size on the
  host PC** — it does NOT reduce WASM binary size at all, since the WASM binary
  only contains integer IDs regardless of how the JSON is organized
- Cost: apps can no longer be built/deployed independently, ID offset
  coordination required, more complex decoder setup

The per-app approach is correct: each app gets its own dictionary, can be
updated independently, and the WASM binary size (the thing that matters on
the embedded device) is unaffected by dictionary organization.

## Limitations

### PRI Format Macros Not Supported

The string extraction tool works on **raw source** (not preprocessed). C format
macros like `PRIu32`, `PRId64`, `PRIx32` from `<inttypes.h>` are not string
literals — they expand during preprocessing. The tool cannot resolve them.

```c
/* NOT supported — will be skipped with a warning: */
LOG_INF("value: %" PRIu32, val);

/* Use direct format specifiers instead: */
LOG_INF("value: %u", val);      /* uint32_t on wasm32 is always %u */
LOG_INF("big: %llu", val64);    /* uint64_t on wasm32 is always %llu */
```

This is acceptable for WASM because types are fixed on the `wasm32` target:
`uint32_t` = `%u`, `int32_t` = `%d`, `uint64_t` = `%llu`, `int64_t` = `%lld`.
There's no portability ambiguity that PRI macros are meant to solve.

Unsupported LOG calls are flagged at build time with a clear warning and
replaced with a comment in the transformed source. The build continues with
the remaining valid calls.

### Max 8 Arguments Per Log Call

The type descriptor is a uint32 with 4 bits per argument, limiting each log
call to 8 typed arguments maximum. Calls exceeding this are skipped with a
warning at build time.

### String Argument Length Limit

String arguments (`%s`) are packed into the binary packet as length-prefixed
data: `[type=0x04][len:2B LE][string bytes]`. The total packet size is capped
at 256 bytes (`WASM_LOG_DICT_MAX_PACKET`). After the 14-byte header and other
args, this leaves roughly 200 bytes for string content. Strings exceeding the
remaining space are truncated at runtime (no build-time warning — the string
value isn't known until runtime).

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

The dictionary logging transformation does NOT weaken the WASM sandbox.
A malicious WASM app controls `log_level`, `string_id`, `arg_type_descriptor`,
and `va_args` — but none of these provide an escape vector:

| Attack Vector | Why It Fails |
|---------------|-------------|
| Read host memory via string offset | `validate_app_str_addr()` bounds-checks within WASM linear memory |
| Read past va_args bounds | `wasm_runtime_get_native_addr_range()` enforces limits |
| Buffer overflow in packet | Every arg write checked against `WASM_LOG_DICT_MAX_PACKET` (256B) |
| Forge app_id | Host-assigned via exec_env user_data — WASM has no API to modify it |
| Malicious string_id/type_desc | Only affects offline decoder (Python on host), not runtime memory |

The worst case: a malicious WASM app emits garbage packets that decode to
nonsense — a denial-of-observability, not a sandbox escape. This is equivalent
to the baseline `wasm_log()` path where a malicious app could print misleading
text.

## Tests

Unit tests for the string extraction script:

```bash
cd product-mini/platforms/zephyr/dictionary-log
python3 -m pytest tests/ -v
```

Tests use C fixture files in `tests/` covering valid patterns (basic, multi-line,
all types) and invalid patterns (PRI macros, non-literal format strings, empty
calls, too many args). 47 tests total.

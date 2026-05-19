#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
decode_wasm_log.py - Offline decoder for WASM dictionary log packets.

Reads a Zephyr UART hex log stream and decodes both Zephyr native dictionary
packets and WASM dictionary packets in a single pass.

Usage:
    python3 decode_wasm_log.py \
        --wasm-db build/unified_wasm_dict.json \
        [--zephyr-db build/zephyr/log_dictionary.json] \
        /tmp/serial.log --hex
"""

import argparse
import io
import json
import logging
import os
import re
import struct
import sys

try:
    import colorama
    HAS_COLOR = True
except ImportError:
    HAS_COLOR = False

# Save raw stdout BEFORE any colorama.init() can wrap it.
# Zephyr's log_parser_v3.py calls colorama.init() which installs a
# stripping wrapper on sys.stdout. We bypass this by writing to the
# original unwrapped stdout for our colored output.
_RAW_STDOUT = sys.stdout

logger = logging.getLogger("decode_wasm_log")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Zephyr message types (from log_output_dict.h)
MSG_TYPE_NORMAL = 0x00
MSG_TYPE_DROPPED = 0x01

# WASM dictionary packet type marker (legacy V1, still used by decode_log_stream
# to identify standalone WASM packets in the binary stream)
MSG_WASM_LOG = 0x80

# Two color schemes to match Zephyr's two different output modes:
#
# Dict ON (binary mode, decoded by log_parser.py):
#   ERR = red, WRN = yellow, INF = green, DBG = blue
#
# Dict OFF (text mode backend):
#   ERR = bright red, WRN = bright yellow, INF = white, DBG = white

WASM_LOG_LEVELS_DICT_ON = {
    1: ("err", "\x1b[31m"),         # red (matches log_parser.py)
    2: ("wrn", "\x1b[33m"),         # yellow (matches log_parser.py)
    3: ("inf", "\x1b[32m"),         # green (matches log_parser.py)
    4: ("dbg", "\x1b[34m"),         # blue (matches log_parser.py)
    5: ("verbose", "\x1b[34m"),     # blue
}

WASM_LOG_LEVELS_DICT_OFF = {
    1: ("err", "\x1b[1;31m"),       # bright red (matches text backend)
    2: ("wrn", "\x1b[1;33m"),       # bright yellow (matches text backend)
    3: ("inf", ""),                  # no color / white (matches text backend)
    4: ("dbg", ""),                  # no color / white (matches text backend)
    5: ("verbose", ""),
}

WASM_LOG_LEVELS_NO_COLOR = {
    1: ("err", ""),
    2: ("wrn", ""),
    3: ("inf", ""),
    4: ("dbg", ""),
    5: ("verbose", ""),
}

# Active color table — selected at runtime based on mode
WASM_LOG_LEVELS = WASM_LOG_LEVELS_NO_COLOR if not HAS_COLOR else WASM_LOG_LEVELS_DICT_OFF

# Flag to indicate binary mode (set by main, read by decode_wasm_packet)
_binary_mode = False

# WASM argument type codes (from lib_wasm_dict_log.c)
WASM_LOG_ARG_INT32 = 0x01
WASM_LOG_ARG_INT64 = 0x02
WASM_LOG_ARG_FLOAT64 = 0x03
WASM_LOG_ARG_STRING = 0x04

# ---------------------------------------------------------------------------
# Zephyr native packet parsing (V3 format, 32-bit little-endian target)
# ---------------------------------------------------------------------------

# V3 header after msg_type byte: domain_lvl(1B) + pkg_len(2B) + data_len(2B)
#   + source_id(4B) + timestamp(4B for 32-bit, 8B for 64-bit)
# Total for 32-bit without CONFIG_LOG_TIMESTAMP_64BIT: 1+1+2+2+4+4 = 14 bytes


def calc_zephyr_native_packet_size(data, offset):
    """Calculate the total size of a Zephyr native MSG_NORMAL packet.

    Returns the total packet size (header + payload), or None if the data
    is truncated.
    """
    # Minimum header: type(1) + domain_lvl(1) + pkg_len(2) + data_len(2)
    #                 + source_id(4) + timestamp(4) = 14 bytes
    if offset + 14 > len(data):
        return None

    pkg_len = struct.unpack_from("<H", data, offset + 2)[0]
    data_len = struct.unpack_from("<H", data, offset + 4)[0]

    total = 14 + pkg_len + data_len
    if offset + total > len(data):
        return None

    return total


def identify_wasm_packet(data, offset, source_map):
    """Check if a Zephyr native packet at offset contains a WASM dict payload.

    Uses the source field to determine if the packet came from 'wasm_dict' module.

    Args:
        data: binary data buffer
        offset: start of the Zephyr native packet (msg_type byte)
        source_map: dict mapping source_id (int) -> module_name (str)

    Returns:
        (wasm_payload_bytes, timestamp_ms) if this is a wasm_dict packet,
        None otherwise.
    """
    if offset + 14 > len(data):
        return None

    pkg_len = struct.unpack_from("<H", data, offset + 2)[0]
    data_len = struct.unpack_from("<H", data, offset + 4)[0]
    source_id = struct.unpack_from("<I", data, offset + 6)[0]
    timestamp = struct.unpack_from("<I", data, offset + 10)[0]

    if data_len == 0:
        return None

    module_name = source_map.get(source_id)
    if module_name != "wasm_dict":
        return None

    data_start = offset + 14 + pkg_len
    if data_start + data_len > len(data):
        return None

    wasm_payload = data[data_start:data_start + data_len]
    return wasm_payload, timestamp


def build_source_map(zephyr_db_path):
    """Build a source_id -> module_name mapping from Zephyr log dictionary.

    The Zephyr log_dictionary.json has: log_subsys.log_instances.{source_id: {name: "..."}}
    """
    try:
        with open(zephyr_db_path, 'r') as f:
            db = json.load(f)
    except (IOError, json.JSONDecodeError) as exc:
        logger.error("Failed to load Zephyr dictionary %s: %s", zephyr_db_path, exc)
        return {}

    source_map = {}
    log_instances = db.get('log_subsys', {}).get('log_instances', {})
    for src_id_str, info in log_instances.items():
        try:
            source_map[int(src_id_str)] = info['name']
        except (KeyError, ValueError):
            continue

    return source_map


# ---------------------------------------------------------------------------
# WASM dictionary packet decoding
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Timestamp format detection and formatting
# ---------------------------------------------------------------------------

# Patterns for auto-detecting timestamp format from native log lines
TS_FORMAT_UPTIME = "uptime"       # [HH:MM:SS.mmm,uuu]
TS_FORMAT_RTC = "rtc"             # [YYYY-MM-DD HH:MM:SS.mmm]
TS_FORMAT_RAW = "raw"             # [     12345] (raw integer)

TS_PATTERN_UPTIME = re.compile(r'\[(\d{2}:\d{2}:\d{2}\.\d{3},\d{3})\]')
TS_PATTERN_RTC = re.compile(r'\[(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\]')
TS_PATTERN_RAW = re.compile(r'\[\s*(\d+)\]')


def detect_timestamp_format(content):
    """Auto-detect timestamp format from native log lines in the content.

    Scans the first few log-like lines and returns the detected format type.
    Returns one of: TS_FORMAT_UPTIME, TS_FORMAT_RTC, TS_FORMAT_RAW.
    """
    for line in content.split('\n')[:100]:
        if '<inf>' in line or '<err>' in line or '<wrn>' in line or '<dbg>' in line:
            if TS_PATTERN_UPTIME.search(line):
                return TS_FORMAT_UPTIME
            if TS_PATTERN_RTC.search(line):
                return TS_FORMAT_RTC
            if TS_PATTERN_RAW.search(line):
                return TS_FORMAT_RAW
    # Default fallback
    return TS_FORMAT_UPTIME


def format_timestamp(timestamp_ms, ts_format):
    """Format a raw millisecond timestamp to match the detected log format.

    Args:
        timestamp_ms: raw timestamp in milliseconds (from k_uptime_get_32)
        ts_format: one of TS_FORMAT_UPTIME, TS_FORMAT_RTC, TS_FORMAT_RAW

    Returns: formatted timestamp string including brackets.
    """
    if ts_format == TS_FORMAT_UPTIME:
        ts_sec = timestamp_ms // 1000
        ts_ms = timestamp_ms % 1000
        ts_h = ts_sec // 3600
        ts_m = (ts_sec % 3600) // 60
        ts_s = ts_sec % 60
        return f"[{ts_h:02d}:{ts_m:02d}:{ts_s:02d}.{ts_ms:03d},000]"

    elif ts_format == TS_FORMAT_RAW:
        return f"[{timestamp_ms:>10}]"

    # Fallback (includes RTC — render as uptime since we can't reconstruct wall clock)
    ts_sec = timestamp_ms // 1000
    ts_ms = timestamp_ms % 1000
    ts_h = ts_sec // 3600
    ts_m = (ts_sec % 3600) // 60
    ts_s = ts_sec % 60
    return f"[{ts_h:02d}:{ts_m:02d}:{ts_s:02d}.{ts_ms:03d},000]"


# Module-level state for timestamp format and color (set during decode)
_ts_format = TS_FORMAT_UPTIME
_native_has_color = False  # whether native log lines have ANSI codes


def detect_native_color(content):
    """Check if native log lines contain ANSI escape codes.

    If they do, our decoded lines should also have colors (matching Zephyr's
    color scheme). If they don't (plain text file capture), decoded lines
    should be white/plain to stay consistent.
    """
    for line in content.split('\n')[:100]:
        if '<inf>' in line or '<err>' in line or '<wrn>' in line or '<dbg>' in line:
            if '\x1b[' in line:
                return True
    return False


# Regex to extract ANSI color code before a log level tag
_ANSI_BEFORE_LEVEL = re.compile(r'(\x1b\[[0-9;]*m)\s*<(err|wrn|inf|dbg)>')


def detect_native_colors_per_level(content):
    """Auto-detect ANSI color codes used by native logs for each level.

    Scans native log lines and extracts the ANSI escape sequence that
    precedes each level tag. Returns a dict: {level_name: ansi_code}.
    If a level is not found or has no color, its value is "".

    This allows matching whatever color scheme Zephyr is configured with,
    even if it differs from the default.
    """
    colors = {"err": "", "wrn": "", "inf": "", "dbg": ""}
    found = set()

    for line in content.split('\n')[:200]:
        if len(found) == 4:
            break
        m = _ANSI_BEFORE_LEVEL.search(line)
        if m:
            ansi_code = m.group(1)
            level = m.group(2)
            if level not in found:
                # \x1b[0m is "reset" = no color (default/white)
                if ansi_code == '\x1b[0m':
                    colors[level] = ""
                else:
                    colors[level] = ansi_code
                found.add(level)

    return colors


# Module-level detected color map (set during init)
_detected_colors = {"err": "", "wrn": "", "inf": "", "dbg": ""}
_auto_color = False  # whether to use auto-detected colors


def init_timestamp_format(content, auto_color=False):
    """Detect and initialize timestamp format and color mode from log content.

    Args:
        content: the full log file content
        auto_color: if True, auto-detect per-level colors from native logs
                    instead of using hardcoded defaults
    """
    global _ts_format, _native_has_color, _detected_colors, _auto_color
    _ts_format = detect_timestamp_format(content)
    _native_has_color = detect_native_color(content)
    _auto_color = auto_color

    if _native_has_color and auto_color:
        _detected_colors = detect_native_colors_per_level(content)
        logger.debug("Auto-detected colors: %s",
                     {k: repr(v) for k, v in _detected_colors.items()})

    logger.debug("Detected timestamp format: %s, native colors: %s, auto_color: %s",
                 _ts_format, _native_has_color, auto_color)


def decode_wasm_packet(data, offset, wasm_dbs, use_color=True):
    """Decode a single WASM dictionary V2 packet starting at offset.

    V2 header (5 bytes): [app_id:1B][level:1B][string_id:2B LE][arg_count:1B]

    Returns (decoded_message, bytes_consumed) or (None, bytes_consumed) on error.
    The decoded_message is the message portion only (level + app_name + text),
    without a timestamp prefix. The caller prepends the timestamp.

    wasm_dbs is a dict mapping app_id -> (dictionary, app_name).
    use_color: if False, don't apply ANSI colors (for text mode merging).
    """
    if offset + 5 > len(data):
        logger.debug("Truncated WASM packet header at offset %d", offset)
        return None, len(data) - offset

    app_id = data[offset]
    log_level = data[offset + 1]
    string_id = struct.unpack_from("<H", data, offset + 2)[0]
    arg_count = data[offset + 4]

    pos = offset + 5

    # Decode arguments
    args = []
    for i in range(arg_count):
        if pos >= len(data):
            logger.warning(
                "Truncated WASM packet at arg %d/%d (offset %d)",
                i, arg_count, offset,
            )
            return None, pos - offset

        atype = data[pos]
        pos += 1

        if atype == WASM_LOG_ARG_INT32:
            if pos + 4 > len(data):
                logger.warning("Truncated int32 arg at offset %d", pos)
                return None, pos - offset
            val = struct.unpack_from("<i", data, pos)[0]
            args.append(val)
            pos += 4

        elif atype == WASM_LOG_ARG_INT64:
            if pos + 8 > len(data):
                logger.warning("Truncated int64 arg at offset %d", pos)
                return None, pos - offset
            val = struct.unpack_from("<q", data, pos)[0]
            args.append(val)
            pos += 8

        elif atype == WASM_LOG_ARG_FLOAT64:
            if pos + 8 > len(data):
                logger.warning("Truncated float64 arg at offset %d", pos)
                return None, pos - offset
            val = struct.unpack_from("<d", data, pos)[0]
            args.append(val)
            pos += 8

        elif atype == WASM_LOG_ARG_STRING:
            if pos + 2 > len(data):
                logger.warning("Truncated string length at offset %d", pos)
                return None, pos - offset
            slen = struct.unpack_from("<H", data, pos)[0]
            pos += 2
            if pos + slen > len(data):
                logger.warning("Truncated string data at offset %d", pos)
                return None, pos - offset
            val = data[pos:pos + slen].decode("utf-8", errors="replace")
            args.append(val)
            pos += slen

        else:
            logger.warning(
                "Unknown arg type 0x%02x at offset %d in WASM packet",
                atype, pos - 1,
            )
            return None, pos - offset

    bytes_consumed = pos - offset

    # Look up the format string
    if app_id not in wasm_dbs:
        logger.warning(
            "Unknown app_id %d in WASM packet at offset %d", app_id, offset
        )
        return None, bytes_consumed

    db, app_name = wasm_dbs[app_id]
    str_key = str(string_id)
    if str_key not in db:
        logger.warning(
            "Unknown string_id %d for app_id %d at offset %d",
            string_id, app_id, offset,
        )
        return None, bytes_consumed

    entry = db[str_key]
    fmt = entry["fmt"]

    # Python's % operator doesn't support %p — convert to hex output
    # Use negative lookbehind to avoid matching the second % of %%
    fmt = re.sub(r'(?<!%)%([-+ #0]*\*?\d*\.?\*?\d*)p', r'%\g<1>#x', fmt)

    # Select color table based on mode and apply only if native logs are colored
    if _binary_mode:
        level_table = WASM_LOG_LEVELS_DICT_ON if HAS_COLOR else WASM_LOG_LEVELS_NO_COLOR
    else:
        level_table = WASM_LOG_LEVELS_DICT_OFF if HAS_COLOR else WASM_LOG_LEVELS_NO_COLOR

    level_info = level_table.get(log_level, ("lvl%d" % log_level, ""))
    level_str, color = level_info
    apply_color = use_color and _native_has_color
    if apply_color and _auto_color:
        color = _detected_colors.get(level_str, "")
    elif not apply_color:
        color = ""
    reset = "\x1b[0m" if (apply_color and color) else ""

    try:
        message = fmt % tuple(args)
    except (TypeError, ValueError) as exc:
        logger.warning(
            "Format error for string_id=%d fmt=%r args=%r: %s",
            string_id, fmt, args, exc,
        )
        message = f"[RAW] id={string_id} fmt={fmt!r} args={args!r}"

    # Return message portion only (no timestamp — caller prepends it)
    msg = f"{color}<{level_str}> {app_name}: {message}{reset}"
    return msg, bytes_consumed


# ---------------------------------------------------------------------------
# Zephyr parser integration (best-effort)
# ---------------------------------------------------------------------------


def try_import_zephyr_parser():
    """Try to import Zephyr's dictionary parser.

    Searches for the parser scripts in:
    1. $ZEPHYR_BASE/scripts/logging/dictionary/
    2. Common install paths: ~/zephyrproject/zephyr/scripts/logging/dictionary/

    Returns a module/class that can parse native packets, or None.
    """
    candidates = []

    zephyr_base = os.environ.get("ZEPHYR_BASE")
    if zephyr_base:
        candidates.append(
            os.path.join(zephyr_base, "scripts", "logging", "dictionary")
        )

    home = os.path.expanduser("~")
    candidates.append(
        os.path.join(home, "zephyrproject", "zephyr", "scripts", "logging", "dictionary")
    )

    parser_dir = None
    for candidate in candidates:
        if os.path.isdir(candidate):
            parser_dir = candidate
            break

    if parser_dir is None:
        logger.debug(
            "Zephyr parser directory not found in any of: %s", candidates
        )
        return None

    logger.debug("Using Zephyr parser from: %s", parser_dir)

    try:
        if parser_dir not in sys.path:
            sys.path.insert(0, parser_dir)
        from dictionary_parser import log_database
        return log_database
    except ImportError as exc:
        logger.debug("Failed to import Zephyr dictionary parser: %s", exc)
        return None


def create_zephyr_parser(zephyr_db_path):
    """Create a Zephyr log parser instance if possible.

    Returns a parser object with a parse_one_msg method, or None.
    """
    log_database_mod = try_import_zephyr_parser()
    if log_database_mod is None:
        return None

    if not os.path.isfile(zephyr_db_path):
        logger.warning("Zephyr database file not found: %s", zephyr_db_path)
        return None

    try:
        database = log_database_mod.LogDatabase.read_json_database(zephyr_db_path)
        if database is None:
            logger.warning("Failed to read Zephyr database: %s", zephyr_db_path)
            return None

        # Import the appropriate version parser
        ver = database.get_version()
        if ver == 3:
            from dictionary_parser.log_parser_v3 import LogParserV3
            return LogParserV3(database)
        elif ver == 1:
            from dictionary_parser.log_parser_v1 import LogParserV1
            return LogParserV1(database)
        else:
            logger.warning("Unsupported Zephyr database version: %d", ver)
            return None
    except Exception as exc:
        logger.debug("Failed to create Zephyr parser: %s", exc)
        return None


# ---------------------------------------------------------------------------
# Main decoding loop
# ---------------------------------------------------------------------------


def extract_wasm_from_text_hexdump(content):
    """Extract WASM dict packets from text-mode hexdump output (dict OFF).

    When Zephyr dictionary logging is OFF, LOG_HEXDUMP produces text like:
        [00:00:00.100,000] <inf> wasm_dict:
          80 00 03 00 00 64 00 00  00 00 00 00 00 00 01 01 |.....d..........|
          2a 00 00 00                                       |*...            |

    This function finds those hexdump blocks and reconstructs binary packets.

    Limitation: if another module logs text containing '> wasm_dict:' at the
    standard module name position, it could produce false positives. This is
    unlikely in practice.
    """
    packets = []
    lines = content.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if '> wasm_dict:' in line:
            hex_bytes = []
            i += 1
            while i < len(lines):
                hex_line = lines[i].strip()
                if not hex_line:
                    i += 1
                    continue
                if '|' in hex_line:
                    hex_line = hex_line[:hex_line.index('|')].strip()
                parts = hex_line.split()
                valid_hex = True
                for part in parts:
                    if len(part) == 2 and all(c in '0123456789abcdefABCDEF' for c in part):
                        hex_bytes.append(int(part, 16))
                    else:
                        valid_hex = False
                        break
                if not valid_hex:
                    break
                i += 1
            if hex_bytes:
                packets.append(bytes(hex_bytes))
        else:
            i += 1
    return packets


def decode_text_mode(content, wasm_dbs):
    """Decode dict OFF mode: merge native text logs with decoded WASM hexdumps.

    In dict OFF mode, the serial output contains:
    - Human-readable native/baseline logs: [timestamp] <level> module: message
    - LOG_HEXDUMP blocks from wasm_dict module (our binary packets as text hex)

    This function outputs a unified log: native lines pass through as-is,
    wasm_dict hexdump blocks are decoded and replaced with formatted log lines.

    Returns (wasm_count, error_count).
    """
    lines = content.split('\n')
    i = 0
    wasm_count = 0
    error_count = 0

    while i < len(lines):
        line = lines[i]

        # Check if this is a wasm_dict hexdump header line.
        # A hexdump header has NOTHING after "wasm_dict:" (or just whitespace),
        # followed by indented hex lines. If there's text content after the colon
        # (like "My_APP: message"), it's a regular baseline log — pass through.
        if '> wasm_dict:' in line:
            # Check if line has content after "wasm_dict:"
            after_module = line.split('> wasm_dict:', 1)[1].strip()
            # Strip ANSI codes for the check
            after_clean = ANSI_ESCAPE_RE.sub('', after_module).strip()
            if after_clean:
                # Has content after module name — it's a regular text log, pass through
                _RAW_STDOUT.write(line.rstrip() + "\n")
                i += 1
                continue

            # Empty after "wasm_dict:" — this is a hexdump header
            header_line = line  # Save for timestamp extraction
            hex_bytes = []
            i += 1
            while i < len(lines):
                hex_line = lines[i].strip()
                if not hex_line:
                    i += 1
                    continue
                if '|' in hex_line:
                    hex_line = hex_line[:hex_line.index('|')].strip()
                parts = hex_line.split()
                valid_hex = True
                for part in parts:
                    if len(part) == 2 and all(c in '0123456789abcdefABCDEF' for c in part):
                        hex_bytes.append(int(part, 16))
                    else:
                        valid_hex = False
                        break
                if not valid_hex:
                    break
                i += 1

            # Decode the WASM packet and print in place of the hexdump
            if hex_bytes:
                pkt_data = bytes(hex_bytes)
                msg, _ = decode_wasm_packet(pkt_data, 0, wasm_dbs)
                if msg is not None:
                    # Use timestamp from the hexdump header line
                    ts_match = re.search(r'(\[[^\]]+\])', header_line)
                    ts_prefix = ts_match.group(1) + ' ' if ts_match else ''
                    _RAW_STDOUT.write(ts_prefix + msg + "\n")
                    wasm_count += 1
                else:
                    error_count += 1
            # Skip the hexdump header line (don't print it)
        else:
            # Regular text log line — pass through as-is
            if line.strip():
                _RAW_STDOUT.write(line + "\n")
            i += 1

    return wasm_count, error_count


def extract_hex_data(content):
    """Extract hex-encoded log data from UART output.

    Two modes:
    1. ##ZLOGV1## found (Zephyr dict ON): all packets (native + WASM) are after
       the separator in a unified stream.
    2. No separator (Zephyr dict OFF): parse text hexdump from wasm_dict module.

    Returns the binary data, or None if no hex data found.
    """
    marker_idx = content.find("##ZLOGV1##")

    if marker_idx >= 0:
        # All packets (native + WASM) are after the separator in unified stream
        after = content[marker_idx + len("##ZLOGV1##"):]
        hexdata = ""
        for c in after:
            if c in "0123456789abcdefABCDEF":
                hexdata += c
            elif c in "\n\r \t":
                continue
            else:
                break
    else:
        # Mode 2: no separator — Zephyr dict is OFF (text output)
        # WASM packets appear as LOG_HEXDUMP text from wasm_dict module
        logger.debug("No ##ZLOGV1## marker — parsing text hexdump from wasm_dict")
        packets = extract_wasm_from_text_hexdump(content)
        if packets:
            hexdata = ''.join(pkt.hex() for pkt in packets)
        else:
            hexdata = ""

    if not hexdata:
        logger.error("No hex data found in input")
        return None

    if len(hexdata) % 2 != 0:
        hexdata = hexdata[:-1]

    try:
        return bytes.fromhex(hexdata)
    except ValueError as exc:
        logger.error("Invalid hex data: %s", exc)
        return None


TIMESTAMP_RE = re.compile(r"\[\s*(\d+)\]")
ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*m")


def extract_timestamp(line):
    """Extract the integer timestamp from a decoded log line for sorting.
    Strips ANSI color codes before matching."""
    stripped = ANSI_ESCAPE_RE.sub("", line)
    m = TIMESTAMP_RE.search(stripped)
    if m:
        return int(m.group(1))
    return 0


def decode_log_stream(data, wasm_dbs, zephyr_parser=None, sort_output=False,
                      source_map=None):
    """Decode the binary log stream.

    Iterates through packets, dispatching based on msg_type:
    - 0x00 (MSG_NORMAL): Zephyr native packet (may contain embedded WASM payload
      identified by source_map lookup)
    - 0x01 (MSG_DROPPED): Zephyr dropped message notification
    - Others: skip with warning

    If source_map is provided, WASM packets are identified by matching the
    source field against the 'wasm_dict' module in the map. If source_map is
    None or empty, falls back to legacy 0x80 magic byte check.

    If sort_output is True, collects all lines and sorts by timestamp
    before printing. Otherwise prints in packet order.

    wasm_dbs is a dict mapping app_id -> (dictionary, app_name).

    Returns counts: (zephyr_count, wasm_count, skipped_count, error_count)
    """
    if source_map is None:
        source_map = {}

    offset = 0
    zephyr_count = 0
    wasm_count = 0
    skipped_count = 0
    error_count = 0
    lines = [] if sort_output else None

    def emit(text):
        if sort_output:
            lines.append(text)
        else:
            _RAW_STDOUT.write(text)
            if not text.endswith("\n"):
                _RAW_STDOUT.write("\n")

    while offset < len(data):
        msg_type = data[offset]

        if msg_type == MSG_TYPE_NORMAL:
            pkt_size = calc_zephyr_native_packet_size(data, offset)
            if pkt_size is None:
                logger.debug(
                    "Truncated Zephyr packet at offset %d, stopping", offset
                )
                break

            # Check if this native packet contains an embedded WASM dict packet
            wasm_extracted = False
            if source_map:
                # Source-ID based identification
                wasm_result = identify_wasm_packet(data, offset, source_map)
                if wasm_result is not None:
                    wasm_payload, pkt_timestamp = wasm_result
                    msg, _ = decode_wasm_packet(wasm_payload, 0, wasm_dbs)
                    if msg is not None:
                        ts_str = format_timestamp(pkt_timestamp, _ts_format)
                        emit(f"{ts_str} {msg}")
                        wasm_count += 1
                    else:
                        error_count += 1
                    wasm_extracted = True
            else:
                # Legacy fallback: identify by 0x80 magic byte in data field
                if offset + 14 <= len(data):
                    pkg_len = struct.unpack_from("<H", data, offset + 2)[0]
                    data_len = struct.unpack_from("<H", data, offset + 4)[0]
                    data_start = offset + 14 + pkg_len

                    if (data_len > 0
                            and data_start + data_len <= len(data)
                            and data_start < len(data)
                            and data[data_start] == MSG_WASM_LOG):
                        # Extract WASM packet, skipping the 0x80 marker byte
                        wasm_payload = data[data_start + 1:data_start + data_len]
                        line, _ = decode_wasm_packet(wasm_payload, 0, wasm_dbs)
                        if line is not None:
                            emit(line)
                            wasm_count += 1
                        else:
                            error_count += 1
                        wasm_extracted = True

            if not wasm_extracted and zephyr_parser is not None:
                try:
                    buf = io.StringIO()
                    old_stdout = sys.stdout
                    sys.stdout = buf
                    try:
                        _ok, new_offset = zephyr_parser.parse_one_msg(
                            data, offset
                        )
                    finally:
                        sys.stdout = old_stdout
                    captured = buf.getvalue()
                    if captured:
                        for cline in captured.splitlines():
                            if cline:
                                emit(cline)
                    if not _ok:
                        logger.debug(
                            "Zephyr parser failed at offset %d, skipping",
                            offset,
                        )
                except Exception as exc:
                    logger.debug(
                        "Zephyr parser exception at offset %d: %s", offset, exc
                    )

            offset += pkt_size
            zephyr_count += 1

        elif msg_type == MSG_TYPE_DROPPED:
            if offset + 3 > len(data):
                logger.debug("Truncated dropped message at offset %d", offset)
                break
            cnt = struct.unpack_from("<H", data, offset + 1)[0]
            emit(f"--- {cnt} messages dropped ---")
            offset += 3
            zephyr_count += 1

        elif 0x02 <= msg_type <= 0x7F:
            logger.warning(
                "Unknown Zephyr message type 0x%02x at offset %d, skipping 1 byte",
                msg_type, offset,
            )
            offset += 1
            skipped_count += 1

        else:
            logger.debug(
                "Unknown byte 0x%02x at offset %d, skipping", msg_type, offset
            )
            offset += 1
            skipped_count += 1

    if sort_output and lines:
        lines.sort(key=extract_timestamp)
        for line in lines:
            _RAW_STDOUT.write(line + "\n")

    return zephyr_count, wasm_count, skipped_count, error_count


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def parse_wasm_dbs(unified_json_path):
    """Load unified WASM dictionary JSON and return {app_id: (dict, app_name)} mapping."""
    try:
        with open(unified_json_path, 'r') as f:
            data = json.load(f)
    except (IOError, json.JSONDecodeError) as exc:
        logger.error("Failed to load WASM dictionary %s: %s", unified_json_path, exc)
        sys.exit(1)

    if not isinstance(data, list):
        logger.error("WASM dictionary must be a JSON array (unified format): %s",
                     unified_json_path)
        sys.exit(1)

    dbs = {}
    for entry in data:
        app_id = entry["app_id"]
        app_name = entry["app_name"]
        dict_data = entry["dict"]
        dbs[app_id] = (dict_data, app_name)
        logger.debug("Loaded app_id=%d (%s): %d entries", app_id, app_name, len(dict_data))

    return dbs


def main():
    global _ts_format, _native_has_color, WASM_LOG_LEVELS, _binary_mode

    parser = argparse.ArgumentParser(
        description="Decode WASM dictionary log packets from Zephyr UART hex output.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  # Decode WASM packets only
  python3 %(prog)s --wasm-db build/unified_wasm_dict.json /tmp/serial.log --hex

  # Also decode Zephyr native packets (requires ZEPHYR_BASE)
  python3 %(prog)s --wasm-db build/unified_wasm_dict.json \\
      --zephyr-db build/zephyr/log_dictionary.json /tmp/serial.log --hex
""",
    )
    parser.add_argument(
        "--wasm-db",
        required=True,
        help="Path to unified WASM dictionary JSON (output of stitch_wasm_dicts.py)",
    )
    parser.add_argument(
        "--zephyr-db",
        default=None,
        help="Path to the Zephyr log dictionary JSON (log_dictionary.json)",
    )
    parser.add_argument(
        "logfile",
        help="Path to the captured serial/UART log file",
    )
    parser.add_argument(
        "--hex",
        action="store_true",
        help="Input contains ##ZLOGV1## hex-encoded data (default mode for QEMU captures)",
    )
    parser.add_argument(
        "--sort",
        action="store_true",
        help="Sort output by timestamp (needed when WASM and Zephyr packets are in separate hex blocks)",
    )
    parser.add_argument(
        "--auto-color",
        action="store_true",
        help="Auto-detect color scheme from native log lines instead of using "
             "hardcoded defaults. Use this if Zephyr's color config differs from default.",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose/debug output",
    )
    args = parser.parse_args()

    # Configure logging
    log_level = logging.DEBUG if args.verbose else logging.WARNING
    logging.basicConfig(
        level=log_level,
        format="%(levelname)s: %(message)s",
    )

    # Load WASM dictionaries
    wasm_dbs = parse_wasm_dbs(args.wasm_db)

    # Try to set up Zephyr parser
    zephyr_parser = None
    if args.zephyr_db:
        zephyr_parser = create_zephyr_parser(args.zephyr_db)
        if zephyr_parser is not None:
            logger.debug("Zephyr parser initialized successfully")
        else:
            logger.debug(
                "Zephyr parser not available, native packets will be skipped"
            )

    # Read input file
    try:
        with open(args.logfile, "r") as f:
            content = f.read()
    except IOError as exc:
        logger.error("Failed to read log file %s: %s", args.logfile, exc)
        sys.exit(1)

    # Detect mode: if --zephyr-db is provided, use binary dict mode.
    # Otherwise use text mode. ##ZLOGV1## is a secondary indicator.
    binary_mode = args.zephyr_db is not None or "##ZLOGV1##" in content
    has_separator = "##ZLOGV1##" in content

    # Auto-detect timestamp format and optionally color scheme from native log lines.
    # In binary mode, --auto-color is ignored (no text lines to scan; we use
    # log_parser.py's color scheme which is fixed).
    auto_color = args.auto_color and not binary_mode
    init_timestamp_format(content, auto_color=auto_color)

    if binary_mode:
        # Dict ON: Zephyr's log_parser.py uses raw integer format [%10d]
        # and colors (ERR=red, WRN=yellow, INF=green, DBG=blue).
        # --auto-color is forced off — colors are hardcoded to match log_parser.py.
        _ts_format = TS_FORMAT_RAW
        _native_has_color = True
        _binary_mode = True
    else:
        # Dict OFF: text mode
        _binary_mode = False

    if binary_mode and zephyr_parser is None:
        print("Warning: binary dict mode detected (##ZLOGV1## present) but "
              "--zephyr-db not provided.\n"
              "  Native Zephyr log packets will not be decoded.\n"
              "  Add --zephyr-db build/zephyr/log_dictionary.json to see native logs.\n",
              file=sys.stderr)

    if binary_mode:
        # Dict ON (binary): extract binary data and decode packet stream
        data = extract_hex_data(content)
        if data is None:
            sys.exit(1)

        logger.debug("Decoded %d bytes of binary log data", len(data))

        # Build source_map from Zephyr dictionary for source-ID based identification
        source_map = {}
        if args.zephyr_db:
            source_map = build_source_map(args.zephyr_db)
            if source_map:
                logger.debug("Built source map with %d entries", len(source_map))
            else:
                logger.debug("Source map empty, falling back to 0x80 identification")

        zephyr_n, wasm_n, skip_n, err_n = decode_log_stream(
            data, wasm_dbs, zephyr_parser, sort_output=args.sort,
            source_map=source_map
        )

        print(
            f"\n--- Decode summary ---\n"
            f"  Zephyr native packets: {zephyr_n}\n"
            f"  WASM dict packets:     {wasm_n}\n"
            f"  Skipped bytes:         {skip_n}\n"
            f"  Decode errors:         {err_n}\n"
            f"  Total binary bytes:    {len(data)}",
            file=sys.stderr,
        )
    else:
        # Dict OFF: text mode — merge native text logs with decoded WASM hexdumps
        logger.debug("No ##ZLOGV1## — using text mode (dict OFF)")
        wasm_n, err_n = decode_text_mode(content, wasm_dbs)

        print(
            f"\n--- Decode summary (text mode) ---\n"
            f"  WASM dict packets decoded: {wasm_n}\n"
            f"  Decode errors:             {err_n}\n"
            f"  (Native logs passed through as-is)",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
wasm_log_common.py - Shared utilities for WASM dictionary log decoding.

This module contains constants, format detection, and packet decoding functions
used by both the main decoder (decode_wasm_log.py) and related tools.
"""

import json
import logging
import re
import struct
import sys

try:
    import colorama
    HAS_COLOR = True
except ImportError:
    HAS_COLOR = False

logger = logging.getLogger("wasm_log_common")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# WASM argument type codes (from lib_wasm_dict_log.c)
WASM_LOG_ARG_INT32 = 0x01
WASM_LOG_ARG_INT64 = 0x02
WASM_LOG_ARG_FLOAT64 = 0x03
WASM_LOG_ARG_STRING = 0x04

# Timestamp format identifiers
TS_FORMAT_UPTIME = "uptime"       # [HH:MM:SS.mmm,uuu]
TS_FORMAT_RTC = "rtc"             # [YYYY-MM-DD HH:MM:SS.mmm]
TS_FORMAT_RAW = "raw"             # [     12345] (raw integer)

# Patterns for auto-detecting timestamp format from native log lines
TS_PATTERN_UPTIME = re.compile(r'\[(\d{2}:\d{2}:\d{2}\.\d{3},\d{3})\]')
TS_PATTERN_RTC = re.compile(r'\[(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\]')
TS_PATTERN_RAW = re.compile(r'\[\s*(\d+)\]')

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

ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*m")

# Regex to extract ANSI color code before a log level tag
_ANSI_BEFORE_LEVEL = re.compile(r'(\x1b\[[0-9;]*m)\s*<(err|wrn|inf|dbg)>')


# ---------------------------------------------------------------------------
# Timestamp format detection and formatting
# ---------------------------------------------------------------------------


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


# ---------------------------------------------------------------------------
# Native color detection
# ---------------------------------------------------------------------------


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


# ---------------------------------------------------------------------------
# WASM dictionary loading
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


# ---------------------------------------------------------------------------
# WASM dictionary packet decoding
# ---------------------------------------------------------------------------


def decode_wasm_packet(data, offset, wasm_dbs, use_color=True,
                       binary_mode=False, native_has_color=False,
                       auto_color=False, detected_colors=None):
    """Decode a single WASM dictionary V2 packet starting at offset.

    V2 header (5 bytes): [app_id:1B][level:1B][string_id:2B LE][arg_count:1B]

    Returns (decoded_message, bytes_consumed) or (None, bytes_consumed) on error.
    The decoded_message is the message portion only (level + app_name + text),
    without a timestamp prefix. The caller prepends the timestamp.

    wasm_dbs is a dict mapping app_id -> (dictionary, app_name).
    use_color: if False, don't apply ANSI colors (for text mode merging).
    binary_mode: if True, use DICT_ON color scheme; otherwise DICT_OFF.
    native_has_color: if True, native logs have ANSI codes so we should too.
    auto_color: if True, use detected_colors instead of hardcoded defaults.
    detected_colors: dict of {level_name: ansi_code} from auto-detection.
    """
    if detected_colors is None:
        detected_colors = {}

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

    # Determine level string and color
    if binary_mode:
        level_table = WASM_LOG_LEVELS_DICT_ON if HAS_COLOR else WASM_LOG_LEVELS_NO_COLOR
    else:
        level_table = WASM_LOG_LEVELS_DICT_OFF if HAS_COLOR else WASM_LOG_LEVELS_NO_COLOR

    level_info = level_table.get(log_level, ("lvl%d" % log_level, ""))
    level_str, color = level_info
    apply_color = use_color and native_has_color
    if apply_color and auto_color:
        color = detected_colors.get(level_str, "")
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

    # Return: (text, color, reset, bytes_consumed)
    # Caller is responsible for assembling the full colored line (including timestamp)
    text = f"<{level_str}> {app_name}: {message}"
    return (text, color, reset), bytes_consumed

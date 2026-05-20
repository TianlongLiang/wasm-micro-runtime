#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
decode_dict_on.py - Dict ON (binary stream) decoder for WASM dictionary log packets.

Handles the binary packet stream when Zephyr dictionary logging is enabled (Dict ON).
Extracts hex data from UART captures, identifies Zephyr native and WASM packets,
and decodes them in a single pass.

This module is called by decode_wasm_log.py when binary mode is detected.
"""

import io
import json
import logging
import os
import re
import struct
import sys

from wasm_log_common import decode_wasm_packet, format_timestamp

logger = logging.getLogger("decode_dict_on")

# Save raw stdout BEFORE any colorama.init() can wrap it.
_RAW_STDOUT = sys.stdout

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Zephyr message types (from log_output_dict.h)
MSG_TYPE_NORMAL = 0x00
MSG_TYPE_DROPPED = 0x01

# WASM dictionary packet type marker (legacy V1, still used by decode_log_stream
# to identify standalone WASM packets in the binary stream)
MSG_WASM_LOG = 0x80

TIMESTAMP_RE = re.compile(r"\[\s*(\d+)\]")
ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*m")


# ---------------------------------------------------------------------------
# Zephyr native packet parsing (V3 format, 32-bit little-endian target)
# ---------------------------------------------------------------------------


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
# Hex data extraction
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


# ---------------------------------------------------------------------------
# Timestamp extraction for sorting
# ---------------------------------------------------------------------------


def extract_timestamp(line):
    """Extract the integer timestamp from a decoded log line for sorting.
    Strips ANSI color codes before matching."""
    stripped = ANSI_ESCAPE_RE.sub("", line)
    m = TIMESTAMP_RE.search(stripped)
    if m:
        return int(m.group(1))
    return 0


# ---------------------------------------------------------------------------
# Main decoding loop
# ---------------------------------------------------------------------------


def decode_log_stream(data, wasm_dbs, zephyr_parser=None, sort_output=False,
                      source_map=None, decode_state=None):
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
    decode_state: object with ts_format, native_has_color, binary_mode,
                  auto_color, detected_colors attributes.

    Returns counts: (zephyr_count, wasm_count, skipped_count, error_count)
    """
    if source_map is None:
        source_map = {}

    # Extract decode_state attributes (with fallback defaults for backward compat)
    if decode_state is not None:
        ts_format = decode_state.ts_format
        native_has_color = decode_state.native_has_color
        binary_mode = decode_state.binary_mode
        auto_color = decode_state.auto_color
        detected_colors = decode_state.detected_colors
    else:
        from wasm_log_common import TS_FORMAT_UPTIME
        ts_format = TS_FORMAT_UPTIME
        native_has_color = False
        binary_mode = False
        auto_color = False
        detected_colors = {"err": "", "wrn": "", "inf": "", "dbg": ""}

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
                    result, _ = decode_wasm_packet(
                        wasm_payload, 0, wasm_dbs,
                        binary_mode=binary_mode,
                        native_has_color=native_has_color,
                        auto_color=auto_color,
                        detected_colors=detected_colors,
                    )
                    if result is not None:
                        text, color, reset = result
                        ts_str = format_timestamp(pkt_timestamp, ts_format)
                        emit(f"{color}{ts_str} {text}{reset}")
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
                        result, _ = decode_wasm_packet(
                            wasm_payload, 0, wasm_dbs,
                            binary_mode=binary_mode,
                            native_has_color=native_has_color,
                            auto_color=auto_color,
                            detected_colors=detected_colors,
                        )
                        if result is not None:
                            text, color, reset = result
                            ts_str = format_timestamp(
                                struct.unpack_from("<I", data, offset + 10)[0],
                                ts_format)
                            emit(f"{color}{ts_str} {text}{reset}")
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


def decode(content, wasm_dbs, zephyr_db_path, decode_state, sort_output=False):
    """Dict ON binary decode entry point.

    Args:
        content: raw log file content string
        wasm_dbs: {app_id: (dict, app_name)} from parse_wasm_dbs
        zephyr_db_path: path to log_dictionary.json (or None)
        decode_state: object with ts_format, native_has_color, binary_mode, auto_color, detected_colors
        sort_output: whether to sort output by timestamp

    Returns: (zephyr_count, wasm_count, skipped_count, error_count)
    """
    data = extract_hex_data(content)
    if data is None:
        sys.exit(1)

    logger.debug("Decoded %d bytes of binary log data", len(data))

    source_map = {}
    zephyr_parser = None
    if zephyr_db_path:
        source_map = build_source_map(zephyr_db_path)
        zephyr_parser = create_zephyr_parser(zephyr_db_path)
        if source_map:
            logger.debug("Built source map with %d entries", len(source_map))

    return decode_log_stream(data, wasm_dbs, zephyr_parser,
                             sort_output=sort_output, source_map=source_map,
                             decode_state=decode_state)

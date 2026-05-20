#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Dict OFF text mode decoder for WASM dictionary log packets.

This module handles the case where Zephyr dictionary logging is OFF.
WASM packets appear as LOG_HEXDUMP text blocks from the 'wasm_dict' module.
This module can be deleted when Dict OFF support is removed.
"""

import re
import sys

from wasm_log_common import decode_wasm_packet, ANSI_ESCAPE_RE

_RAW_STDOUT = sys.stdout


def decode(content, wasm_dbs, decode_state):
    """Decode dict OFF mode: merge native text logs with decoded WASM hexdumps.

    Args:
        content: full log file content (string)
        wasm_dbs: {app_id: (dict, app_name)}
        decode_state: object with binary_mode, native_has_color, auto_color, detected_colors

    Returns (wasm_count, error_count).
    """
    lines = content.split('\n')
    i = 0
    wasm_count = 0
    error_count = 0

    while i < len(lines):
        line = lines[i]

        if '> wasm_dict:' in line:
            after_module = line.split('> wasm_dict:', 1)[1].strip()
            after_clean = ANSI_ESCAPE_RE.sub('', after_module).strip()
            if after_clean:
                _RAW_STDOUT.write(line.rstrip() + "\n")
                i += 1
                continue

            header_line = line
            hex_bytes, i = _collect_hex_bytes(lines, i + 1)

            if hex_bytes:
                pkt_data = bytes(hex_bytes)
                result, _ = decode_wasm_packet(
                    pkt_data, 0, wasm_dbs,
                    binary_mode=decode_state.binary_mode,
                    native_has_color=decode_state.native_has_color,
                    auto_color=decode_state.auto_color,
                    detected_colors=decode_state.detected_colors,
                )
                if result is not None:
                    text, color, reset = result
                    ts_prefix = _extract_timestamp_from_line(header_line)
                    _RAW_STDOUT.write(f"{color}{ts_prefix}{text}{reset}\n")
                    wasm_count += 1
                else:
                    error_count += 1
        else:
            if line.strip():
                _RAW_STDOUT.write(line + "\n")
            i += 1

    return wasm_count, error_count


def _collect_hex_bytes(lines, start_index):
    """Collect hex bytes from indented hexdump lines.

    Returns (list_of_int_bytes, next_line_index).
    """
    hex_bytes = []
    i = start_index
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
    return hex_bytes, i


def _extract_timestamp_from_line(line):
    """Extract [timestamp] prefix from a text log line."""
    ts_match = re.search(r'(\[[^\]]+\])', line)
    return ts_match.group(1) + ' ' if ts_match else ''

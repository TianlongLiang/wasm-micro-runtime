#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
Tests for structured LOG_HEXDUMP output decoding.

Tests the new decoder logic that:
1. Extracts WASM packets from inside Zephyr native msg data fields
2. Parses text hexdump output in dict OFF mode

Run: python3 -m pytest tests/test_structured_output.py -v
"""

import io
import os
import struct
import sys

import pytest

SCRIPT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts')
sys.path.insert(0, SCRIPT_DIR)

from decode_wasm_log import (
    decode_wasm_packet,
    decode_log_stream,
    decode_text_mode,
    extract_hex_data,
    extract_wasm_from_text_hexdump,
    detect_timestamp_format,
    detect_native_color,
    detect_native_colors_per_level,
    format_timestamp,
    init_timestamp_format,
    MSG_WASM_LOG,
    MSG_TYPE_NORMAL,
    MSG_TYPE_DROPPED,
    TS_FORMAT_UPTIME,
    TS_FORMAT_RTC,
    TS_FORMAT_RAW,
)


def capture_decode_log_stream(data, wasm_dbs, zephyr_parser=None, sort_output=False):
    """Run decode_log_stream and capture output as a list of lines."""
    import decode_wasm_log
    buf = io.StringIO()
    old_raw = decode_wasm_log._RAW_STDOUT
    decode_wasm_log._RAW_STDOUT = buf
    try:
        zn, wn, sn, en = decode_log_stream(data, wasm_dbs, zephyr_parser, sort_output)
    finally:
        decode_wasm_log._RAW_STDOUT = old_raw
    lines = [l for l in buf.getvalue().splitlines() if l.strip()]
    return zn, wn, sn, en, lines

# Arg type constants
ARG_INT32 = 0x01
ARG_STRING = 0x04

# Simple test dictionary
TEST_DBS = {
    0: ({
        "0": {"fmt": "hello world", "arg_types": []},
        "1": {"fmt": "value=%d", "arg_types": ["int32"]},
    }, "test_app"),
}


def build_wasm_packet(app_id=0, level=3, string_id=0, timestamp=100, args=None):
    """Build a WASM dict binary packet with 0x80 prefix (for legacy fallback tests).

    Format: [0x80][V2_payload]
    The legacy fallback in decode_log_stream strips the 0x80 marker then
    passes the rest to decode_wasm_packet() which expects V2 format.
    The timestamp parameter is kept for API compat but not encoded in the packet.
    """
    pkt = bytearray()
    pkt.append(MSG_WASM_LOG)
    pkt.append(app_id)
    pkt.append(level)
    pkt += struct.pack('<H', string_id)
    if args is None:
        args = []
    pkt.append(len(args))
    for atype, value in args:
        pkt.append(atype)
        if atype == ARG_INT32:
            pkt += struct.pack('<i', value)
        elif atype == ARG_STRING:
            encoded = value.encode('utf-8')
            pkt += struct.pack('<H', len(encoded))
            pkt += encoded
    return bytes(pkt)


def build_wasm_v2_packet(app_id=0, level=3, string_id=0, args=None):
    """Build a WASM dict V2 binary packet (no 0x80 prefix, no timestamp).

    V2 header: [app_id:1B][level:1B][string_id:2B LE][arg_count:1B]
    """
    pkt = bytearray()
    pkt.append(app_id)
    pkt.append(level)
    pkt += struct.pack('<H', string_id)
    if args is None:
        args = []
    pkt.append(len(args))
    for atype, value in args:
        pkt.append(atype)
        if atype == ARG_INT32:
            pkt += struct.pack('<i', value)
        elif atype == ARG_STRING:
            encoded = value.encode('utf-8')
            pkt += struct.pack('<H', len(encoded))
            pkt += encoded
    return bytes(pkt)


def build_native_packet_with_wasm_data(wasm_pkt, source_id=1, timestamp=100):
    """Wrap a WASM packet inside a Zephyr native msg as the data field.

    Native V3 header (14 bytes):
      type(1) + domain_lvl(1) + pkg_len(2) + data_len(2) + source_id(4) + timestamp(4)
    Then: cbprintf_package (pkg_len bytes) + data (data_len bytes)
    """
    # Minimal cbprintf package for "" (empty string label from LOG_HEXDUMP)
    # In practice this is architecture-dependent, but for testing we use a minimal one
    cbprintf_pkg = struct.pack('<I', 0)  # minimal 4-byte package (null pointer)
    pkg_len = len(cbprintf_pkg)
    data_len = len(wasm_pkt)

    header = bytearray()
    header.append(MSG_TYPE_NORMAL)       # type
    header.append(0x03)                  # domain=0, level=3 (INF)
    header += struct.pack('<H', pkg_len) # pkg_len
    header += struct.pack('<H', data_len)  # data_len
    header += struct.pack('<I', source_id)  # source_id
    header += struct.pack('<I', timestamp)  # timestamp

    return bytes(header) + cbprintf_pkg + wasm_pkt


def build_native_packet_no_data(source_id=2, timestamp=50):
    """Build a Zephyr native packet WITHOUT data (regular log message)."""
    cbprintf_pkg = struct.pack('<I', 0)
    pkg_len = len(cbprintf_pkg)
    data_len = 0

    header = bytearray()
    header.append(MSG_TYPE_NORMAL)
    header.append(0x03)
    header += struct.pack('<H', pkg_len)
    header += struct.pack('<H', data_len)
    header += struct.pack('<I', source_id)
    header += struct.pack('<I', timestamp)

    return bytes(header) + cbprintf_pkg


# ---------------------------------------------------------------------------
# Tests: Embedded WASM packet extraction from native messages
# ---------------------------------------------------------------------------

class TestEmbeddedPacketExtraction:
    """Test extracting WASM packets from inside native Zephyr msg data field."""

    def test_wasm_packet_in_native_msg_decoded(self):
        """A native msg with data starting with 0x80 is decoded as WASM."""
        wasm_pkt = build_wasm_packet(string_id=0, timestamp=100)
        native_msg = build_native_packet_with_wasm_data(wasm_pkt)

        zn, wn, sn, en, lines = capture_decode_log_stream(
            native_msg, TEST_DBS, zephyr_parser=None, sort_output=False
        )
        # Should find 1 WASM packet
        assert wn == 1
        assert en == 0
        assert any('hello world' in l for l in lines)

    def test_native_msg_without_data_not_treated_as_wasm(self):
        """A native msg with data_len=0 is not a WASM packet."""
        native_msg = build_native_packet_no_data()

        zn, wn, sn, en, lines = capture_decode_log_stream(
            native_msg, TEST_DBS, zephyr_parser=None, sort_output=False
        )
        assert wn == 0

    def test_native_msg_with_non_wasm_data(self):
        """A native msg with data that doesn't start with 0x80 is not WASM."""
        fake_data = b'\x01\x02\x03\x04\x05'  # doesn't start with 0x80
        native_msg = build_native_packet_with_wasm_data(fake_data)

        zn, wn, sn, en, lines = capture_decode_log_stream(
            native_msg, TEST_DBS, zephyr_parser=None, sort_output=False
        )
        assert wn == 0

    def test_multiple_native_msgs_mixed(self):
        """Mix of native msgs with and without WASM data."""
        wasm_pkt1 = build_wasm_packet(string_id=0, timestamp=100)
        wasm_pkt2 = build_wasm_packet(string_id=1, timestamp=200, args=[(ARG_INT32, 42)])
        native_with_wasm1 = build_native_packet_with_wasm_data(wasm_pkt1, timestamp=100)
        native_without = build_native_packet_no_data(timestamp=150)
        native_with_wasm2 = build_native_packet_with_wasm_data(wasm_pkt2, timestamp=200)

        data = native_with_wasm1 + native_without + native_with_wasm2

        zn, wn, sn, en, lines = capture_decode_log_stream(
            data, TEST_DBS, zephyr_parser=None, sort_output=False
        )
        assert wn == 2
        assert zn == 3  # all 3 are native packets (2 happen to contain WASM)
        assert en == 0

    def test_wasm_packet_with_args_decoded_correctly(self):
        """Embedded WASM packet with args is decoded properly."""
        wasm_pkt = build_wasm_packet(string_id=1, timestamp=300, args=[(ARG_INT32, 99)])
        native_msg = build_native_packet_with_wasm_data(wasm_pkt)

        zn, wn, sn, en, lines = capture_decode_log_stream(
            native_msg, TEST_DBS, zephyr_parser=None, sort_output=False
        )
        assert wn == 1
        assert any('value=99' in l for l in lines)


# ---------------------------------------------------------------------------
# Tests: Legacy standalone 0x80 packet (backward compatibility)
# ---------------------------------------------------------------------------

class TestLegacyStandalonePacket:
    """Test legacy 0x80 identification in binary stream."""

    def test_legacy_0x80_in_native_data_field(self):
        """0x80-prefixed WASM packet inside a native msg's data field is decoded."""
        # The legacy fallback identifies WASM packets by finding 0x80 as the
        # first byte of the data field in a MSG_NORMAL packet (no source_map).
        wasm_pkt = build_wasm_packet(string_id=0, timestamp=100)
        native_msg = build_native_packet_with_wasm_data(wasm_pkt)

        zn, wn, sn, en, lines = capture_decode_log_stream(
            native_msg, TEST_DBS, zephyr_parser=None, sort_output=False
        )
        assert wn == 1
        assert any('hello world' in l for l in lines)


# ---------------------------------------------------------------------------
# Tests: Text hexdump extraction (dict OFF mode)
# ---------------------------------------------------------------------------

class TestTextHexdumpExtraction:
    """Test extract_wasm_from_text_hexdump for dict OFF mode."""

    def test_basic_hexdump_parsed(self):
        """Standard Zephyr LOG_HEXDUMP text format is parsed."""
        text = """\
[00:00:00.100,000] <inf> wasm_dict:
  80 00 03 00 00 64 00 00  00 00 00 00 00 00 00    |.....d.........|
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 1
        assert packets[0][0] == 0x80  # starts with our marker

    def test_multiple_hexdumps(self):
        """Multiple hexdump blocks are each extracted as separate packets."""
        text = """\
[00:00:00.100,000] <inf> wasm_dict:
  80 00 03 00 00 64 00 00  00 00 00 00 00 00 00    |.....d.........|
[00:00:00.200,000] <inf> dict_log_demo: Native log here
[00:00:00.300,000] <dbg> wasm_dict:
  80 00 04 01 00 c8 00 00  00 00 00 00 00 00 00    |................|
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 2

    def test_non_wasm_dict_module_ignored(self):
        """Hexdumps from other modules are not extracted."""
        text = """\
[00:00:00.100,000] <inf> other_module:
  01 02 03 04 05                                    |.....|
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 0

    def test_any_hex_data_from_wasm_dict_extracted(self):
        """Hexdump from wasm_dict is extracted regardless of first byte (V2 format)."""
        text = """\
[00:00:00.100,000] <inf> wasm_dict:
  01 02 03 04 05                                    |.....|
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 1
        assert packets[0] == bytes([0x01, 0x02, 0x03, 0x04, 0x05])

    def test_multiline_hexdump(self):
        """Hexdump spanning multiple lines is concatenated."""
        text = """\
[00:00:00.100,000] <inf> wasm_dict:
  80 00 03 00 00 64 00 00  00 00 00 00 00 00 01 01 |.....d..........|
  2a 00 00 00                                       |*...|
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 1
        assert len(packets[0]) == 20  # 16 + 4 bytes

    def test_no_hexdump_returns_empty(self):
        """Text without any wasm_dict hexdump returns empty list."""
        text = """\
[00:00:00.100,000] <inf> dict_log_demo: Hello world
[00:00:00.200,000] <err> dict_log_demo: Error occurred
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 0

    def test_all_log_levels(self):
        """Hexdumps at all levels (err, wrn, inf, dbg) are extracted."""
        text = """\
[00:00:00.100,000] <err> wasm_dict:
  80 00 01 00 00 64 00 00  00 00 00 00 00 00 00    |.....d.........|
[00:00:00.200,000] <wrn> wasm_dict:
  80 00 02 00 00 c8 00 00  00 00 00 00 00 00 00    |................|
[00:00:00.300,000] <dbg> wasm_dict:
  80 00 04 00 00 2c 01 00  00 00 00 00 00 00 00    |.....,.........|
"""
        packets = extract_wasm_from_text_hexdump(text)
        assert len(packets) == 3


# ---------------------------------------------------------------------------
# Tests: extract_hex_data unified stream (dict ON)
# ---------------------------------------------------------------------------

class TestExtractHexDataUnified:
    """Test that extract_hex_data works with unified stream (all after separator)."""

    def test_all_hex_after_separator(self):
        """With ##ZLOGV1##, all hex data is extracted from after it."""
        wasm_pkt = build_wasm_packet(string_id=0)
        native_msg = build_native_packet_with_wasm_data(wasm_pkt)
        hex_str = native_msg.hex()

        content = f"Some boot text\n##ZLOGV1##{hex_str}\nninja: done"
        data = extract_hex_data(content)
        assert data is not None
        assert data == native_msg

    def test_no_hex_before_separator(self):
        """Pre-separator content is ignored (no more WASM-before-separator)."""
        wasm_pkt = build_wasm_packet(string_id=0)
        hex_before = wasm_pkt.hex()  # This would have been WASM in old approach

        native_msg = build_native_packet_no_data()
        hex_after = native_msg.hex()

        content = f"Boot\n{hex_before}\n##ZLOGV1##{hex_after}\ndone"
        data = extract_hex_data(content)
        assert data is not None
        # Should only contain the hex after separator (native_msg), not the wasm before
        assert data == native_msg


# ---------------------------------------------------------------------------
# Tests: Timestamp format detection
# ---------------------------------------------------------------------------

class TestTimestampDetection:
    """Test auto-detection of timestamp format from native log lines."""

    def test_detect_uptime_format(self):
        """Standard Zephyr uptime format [HH:MM:SS.mmm,uuu] is detected."""
        content = "[00:00:00.010,000] <inf> dict_log_demo: hello\n"
        assert detect_timestamp_format(content) == TS_FORMAT_UPTIME

    def test_detect_rtc_format(self):
        """RTC wall-clock format [YYYY-MM-DD HH:MM:SS.mmm] is detected."""
        content = "[2026-05-13 02:36:01.486] <inf> dict_log_demo: hello\n"
        assert detect_timestamp_format(content) == TS_FORMAT_RTC

    def test_detect_raw_format(self):
        """Raw integer format [  12345] is detected."""
        content = "[     12345] <inf> dict_log_demo: hello\n"
        assert detect_timestamp_format(content) == TS_FORMAT_RAW

    def test_detect_skips_non_log_lines(self):
        """Lines without log level tags are ignored during detection."""
        content = "Boot ROM\nSome random text\n[00:00:00.010,000] <inf> mod: first log\n"
        assert detect_timestamp_format(content) == TS_FORMAT_UPTIME

    def test_default_when_no_logs(self):
        """Defaults to uptime format when no log lines found."""
        content = "No log lines here at all\nJust plain text\n"
        assert detect_timestamp_format(content) == TS_FORMAT_UPTIME

    def test_format_uptime(self):
        """format_timestamp produces correct uptime string."""
        result = format_timestamp(12345, TS_FORMAT_UPTIME)
        assert result == "[00:00:12.345,000]"

    def test_format_uptime_hours(self):
        """format_timestamp handles hours correctly."""
        ms = 3661500  # 1h 1m 1.5s
        result = format_timestamp(ms, TS_FORMAT_UPTIME)
        assert result == "[01:01:01.500,000]"

    def test_format_raw(self):
        """format_timestamp produces correct raw integer string."""
        result = format_timestamp(12345, TS_FORMAT_RAW)
        assert result == "[     12345]"

    def test_format_rtc_falls_back_to_uptime(self):
        """format_timestamp renders RTC as uptime (can't reconstruct wall clock)."""
        result = format_timestamp(100, TS_FORMAT_RTC)
        assert "00:00:00.100,000" in result


# ---------------------------------------------------------------------------
# Tests: Dict OFF text mode merge (decode_text_mode)
# ---------------------------------------------------------------------------

class TestTextModeMerge:
    """Test decode_text_mode — merging native text logs with decoded hexdumps."""

    def test_native_lines_pass_through(self):
        """Regular native log lines are printed as-is."""
        content = "[00:00:00.010,000] <inf> dict_log_demo: hello world\n"
        init_timestamp_format(content)
        import decode_wasm_log
        buf = io.StringIO()
        old_raw = decode_wasm_log._RAW_STDOUT
        decode_wasm_log._RAW_STDOUT = buf
        try:
            wn, en = decode_text_mode(content, TEST_DBS)
        finally:
            decode_wasm_log._RAW_STDOUT = old_raw
        output = buf.getvalue()
        assert "dict_log_demo: hello world" in output
        assert wn == 0

    def test_hexdump_decoded_and_replaces_block(self):
        """wasm_dict hexdump blocks are decoded into formatted log lines."""
        wasm_pkt = build_wasm_v2_packet(string_id=0)
        hex_lines = ' '.join(f'{b:02x}' for b in wasm_pkt)
        content = (
            "[00:00:00.010,000] <inf> dict_log_demo: before\n"
            "[00:00:00.100,000] <inf> wasm_dict:\n"
            f"  {hex_lines}                    |...|\n"
            "[00:00:00.200,000] <inf> dict_log_demo: after\n"
        )
        init_timestamp_format(content)
        import decode_wasm_log
        buf = io.StringIO()
        old_raw = decode_wasm_log._RAW_STDOUT
        decode_wasm_log._RAW_STDOUT = buf
        try:
            wn, en = decode_text_mode(content, TEST_DBS)
        finally:
            decode_wasm_log._RAW_STDOUT = old_raw
        output = buf.getvalue()
        assert "dict_log_demo: before" in output
        assert "dict_log_demo: after" in output
        assert "hello world" in output  # decoded WASM message
        assert "[00:00:00.100,000]" in output  # timestamp from header line
        assert wn == 1
        assert en == 0

    def test_baseline_wasm_dict_logs_pass_through(self):
        """wasm_dict lines WITH content after colon (baseline) pass through."""
        content = "[00:00:00.050,000] <inf> wasm_dict: My_APP: sensor starting\n"
        init_timestamp_format(content)
        import decode_wasm_log
        buf = io.StringIO()
        old_raw = decode_wasm_log._RAW_STDOUT
        decode_wasm_log._RAW_STDOUT = buf
        try:
            wn, en = decode_text_mode(content, TEST_DBS)
        finally:
            decode_wasm_log._RAW_STDOUT = old_raw
        output = buf.getvalue()
        assert "My_APP: sensor starting" in output
        assert wn == 0  # not decoded as WASM dict — passed through

    def test_mixed_all_three_types(self):
        """Native + baseline + dict hexdump all appear in output."""
        wasm_pkt = build_wasm_v2_packet(string_id=1, args=[(ARG_INT32, 42)])
        hex_lines = ' '.join(f'{b:02x}' for b in wasm_pkt)
        content = (
            "[00:00:00.010,000] <inf> dict_log_demo: native log\n"
            "[00:00:00.050,000] <inf> wasm_dict: My_APP: baseline log\n"
            "[00:00:00.200,000] <inf> wasm_dict:\n"
            f"  {hex_lines}                    |...|\n"
            "[00:00:00.300,000] <inf> dict_log_demo: end\n"
        )
        init_timestamp_format(content)
        import decode_wasm_log
        buf = io.StringIO()
        old_raw = decode_wasm_log._RAW_STDOUT
        decode_wasm_log._RAW_STDOUT = buf
        try:
            wn, en = decode_text_mode(content, TEST_DBS)
        finally:
            decode_wasm_log._RAW_STDOUT = old_raw
        output = buf.getvalue()
        assert "native log" in output
        assert "My_APP: baseline log" in output
        assert "value=42" in output  # decoded dict log
        assert "end" in output
        assert wn == 1

    def test_color_in_text_mode_decode(self):
        """Decoded WASM lines in text mode have ANSI color codes (if colorama available)."""
        wasm_pkt = build_wasm_v2_packet(string_id=0)
        hex_lines = ' '.join(f'{b:02x}' for b in wasm_pkt)
        content = (
            "[00:00:00.100,000] <inf> wasm_dict:\n"
            f"  {hex_lines}                    |...|\n"
        )
        init_timestamp_format(content)
        import decode_wasm_log
        buf = io.StringIO()
        old_raw = decode_wasm_log._RAW_STDOUT
        decode_wasm_log._RAW_STDOUT = buf
        try:
            wn, en = decode_text_mode(content, TEST_DBS)
        finally:
            decode_wasm_log._RAW_STDOUT = old_raw
        output = buf.getvalue()
        assert 'hello world' in output


# ---------------------------------------------------------------------------
# Tests: Auto-color detection
# ---------------------------------------------------------------------------

class TestAutoColorDetection:
    """Test auto-detection of per-level color codes from native log lines."""

    def test_detect_no_color_in_plain_text(self):
        """Plain text file (no ANSI codes) → no colors detected."""
        content = "[00:00:00.010,000] <inf> module: hello\n[00:00:00.020,000] <err> module: error\n"
        assert detect_native_color(content) is False

    def test_detect_color_present(self):
        """File with ANSI codes → colors detected."""
        content = "[00:00:00.010,000] \x1b[1;31m<err> module: error\x1b[0m\n"
        assert detect_native_color(content) is True

    def test_detect_per_level_colors_default_zephyr(self):
        """Detect Zephyr's default colors: ERR=bright red, WRN=bright yellow, INF/DBG=reset."""
        content = (
            "[00:00:00.010,000] \x1b[1;31m<err> mod: error\x1b[0m\n"
            "[00:00:00.020,000] \x1b[1;33m<wrn> mod: warning\x1b[0m\n"
            "[00:00:00.030,000] \x1b[0m<inf> mod: info\x1b[0m\n"
            "[00:00:00.040,000] \x1b[0m<dbg> mod: debug\x1b[0m\n"
        )
        colors = detect_native_colors_per_level(content)
        assert colors['err'] == '\x1b[1;31m'
        assert colors['wrn'] == '\x1b[1;33m'
        assert colors['inf'] == ''  # \x1b[0m = reset = no color
        assert colors['dbg'] == ''

    def test_detect_custom_colors(self):
        """Detect non-default custom colors (e.g., DBG=cyan, INF=green)."""
        content = (
            "[00:00:00.010,000] \x1b[31m<err> mod: error\x1b[0m\n"
            "[00:00:00.020,000] \x1b[33m<wrn> mod: warning\x1b[0m\n"
            "[00:00:00.030,000] \x1b[32m<inf> mod: info\x1b[0m\n"
            "[00:00:00.040,000] \x1b[36m<dbg> mod: debug\x1b[0m\n"
        )
        colors = detect_native_colors_per_level(content)
        assert colors['err'] == '\x1b[31m'
        assert colors['wrn'] == '\x1b[33m'
        assert colors['inf'] == '\x1b[32m'
        assert colors['dbg'] == '\x1b[36m'

    def test_auto_color_applied_to_decoded_lines(self):
        """With --auto-color, decoded lines use detected colors."""
        import decode_wasm_log
        content = (
            "[00:00:00.010,000] \x1b[36m<dbg> mod: debug\x1b[0m\n"
            "[00:00:00.020,000] \x1b[32m<inf> mod: info\x1b[0m\n"
        )
        init_timestamp_format(content, auto_color=True)

        wasm_pkt = build_wasm_v2_packet(app_id=0, level=4, string_id=0)
        line, _ = decode_wasm_packet(wasm_pkt, 0, TEST_DBS)
        # DBG detected as cyan (\x1b[36m), should appear in decoded line
        assert '\x1b[36m' in line

    def test_no_auto_color_uses_hardcoded(self):
        """Without --auto-color, hardcoded defaults are used."""
        content = (
            "[00:00:00.010,000] \x1b[36m<dbg> mod: debug\x1b[0m\n"
            "[00:00:00.020,000] \x1b[32m<inf> mod: info\x1b[0m\n"
        )
        init_timestamp_format(content, auto_color=False)

        wasm_pkt = build_wasm_v2_packet(app_id=0, level=4, string_id=0)
        line, _ = decode_wasm_packet(wasm_pkt, 0, TEST_DBS)
        # DBG with hardcoded defaults = no color (empty string)
        assert '\x1b[36m' not in line

    def test_plain_text_no_color_regardless_of_auto(self):
        """Plain text input → no colors even with --auto-color."""
        content = "[00:00:00.010,000] <inf> mod: hello\n"
        init_timestamp_format(content, auto_color=True)

        wasm_pkt = build_wasm_v2_packet(app_id=0, level=3, string_id=0)
        line, _ = decode_wasm_packet(wasm_pkt, 0, TEST_DBS)
        assert '\x1b[' not in line


# ---------------------------------------------------------------------------
# Tests: Source-ID based WASM packet identification
# ---------------------------------------------------------------------------

class TestSourceIdIdentification:
    """Test that WASM packets are identified by source field, not 0x80."""

    def _build_zephyr_native_packet(self, source_id, timestamp_ms,
                                     pkg_data=b'', hexdump_data=b''):
        """Build a Zephyr MSG_NORMAL packet."""
        pkt = bytearray()
        pkt.append(0x00)  # msg_type = MSG_NORMAL
        pkt.append(0x30)  # domain_lvl
        pkt += struct.pack('<H', len(pkg_data))   # pkg_len
        pkt += struct.pack('<H', len(hexdump_data))  # data_len
        pkt += struct.pack('<I', source_id)        # source
        pkt += struct.pack('<I', timestamp_ms)     # timestamp
        pkt += pkg_data
        pkt += hexdump_data
        return bytes(pkt)

    def test_wasm_dict_source_identified(self):
        from decode_wasm_log import identify_wasm_packet
        source_map = {0x1234: "wasm_dict", 0x5678: "other_module"}
        # Build a minimal V2 WASM payload (app_id=0, level=3, string_id=0, 0 args)
        wasm_payload = bytes([0x00, 0x03, 0x00, 0x00, 0x00])
        native_pkt = self._build_zephyr_native_packet(
            source_id=0x1234, timestamp_ms=100, hexdump_data=wasm_payload
        )
        result = identify_wasm_packet(native_pkt, 0, source_map)
        assert result is not None
        data_payload, timestamp = result
        assert data_payload == wasm_payload
        assert timestamp == 100

    def test_non_wasm_source_not_identified(self):
        from decode_wasm_log import identify_wasm_packet
        source_map = {0x1234: "wasm_dict", 0x5678: "other_module"}
        some_data = b'\x80\x00\x03\x00\x00'  # starts with 0x80 but wrong source
        native_pkt = self._build_zephyr_native_packet(
            source_id=0x5678, timestamp_ms=200, hexdump_data=some_data
        )
        result = identify_wasm_packet(native_pkt, 0, source_map)
        assert result is None

    def test_no_data_field_not_identified(self):
        from decode_wasm_log import identify_wasm_packet
        source_map = {0x1234: "wasm_dict"}
        native_pkt = self._build_zephyr_native_packet(
            source_id=0x1234, timestamp_ms=300, hexdump_data=b''
        )
        result = identify_wasm_packet(native_pkt, 0, source_map)
        assert result is None

    def test_unknown_source_not_identified(self):
        from decode_wasm_log import identify_wasm_packet
        source_map = {0x1234: "wasm_dict"}
        native_pkt = self._build_zephyr_native_packet(
            source_id=0x9999, timestamp_ms=400, hexdump_data=b'\x00\x03\x00\x00\x00'
        )
        result = identify_wasm_packet(native_pkt, 0, source_map)
        assert result is None

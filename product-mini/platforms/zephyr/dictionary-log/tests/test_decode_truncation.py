#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
Unit tests for decode_wasm_log.py — truncation and edge case handling.

Run from the dictionary-log directory:
    python3 -m pytest tests/test_decode_truncation.py -v
"""

import os
import struct
import sys

SCRIPT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts')
sys.path.insert(0, SCRIPT_DIR)

from decode_wasm_log import decode_wasm_packet, MSG_WASM_LOG

# Arg type constants (must match lib_wasm_dict_log.c)
ARG_INT32 = 0x01
ARG_INT64 = 0x02
ARG_FLOAT64 = 0x03
ARG_STRING = 0x04


def build_wasm_packet(app_id=0, level=3, string_id=0, timestamp=100,
                       args=None):
    """Build a valid WASM dict packet for testing."""
    pkt = bytearray()
    pkt.append(MSG_WASM_LOG)         # msg_type
    pkt.append(app_id)               # app_id
    pkt.append(level)                # log_level
    pkt += struct.pack('<H', string_id)  # string_id LE
    pkt += struct.pack('<Q', timestamp)  # timestamp LE
    if args is None:
        args = []
    pkt.append(len(args))            # arg_count

    for atype, value in args:
        pkt.append(atype)
        if atype == ARG_INT32:
            pkt += struct.pack('<i', value)
        elif atype == ARG_INT64:
            pkt += struct.pack('<q', value)
        elif atype == ARG_FLOAT64:
            pkt += struct.pack('<d', value)
        elif atype == ARG_STRING:
            encoded = value.encode('utf-8')
            pkt += struct.pack('<H', len(encoded))
            pkt += encoded

    return bytes(pkt)


# Simple test dictionary
TEST_DB = {
    0: ({
        "0": {"fmt": "hello world", "arg_types": []},
        "1": {"fmt": "value=%d", "arg_types": ["int32"]},
        "2": {"fmt": "name=%s id=%d", "arg_types": ["string", "int32"]},
        "3": {"fmt": "big=%lld", "arg_types": ["int64"]},
        "4": {"fmt": "temp=%f", "arg_types": ["float64"]},
        "5": {"fmt": "long string: %s", "arg_types": ["string"]},
    }, "test_app"),
}


# ---------------------------------------------------------------------------
# Tests: Valid packet decoding
# ---------------------------------------------------------------------------

class TestValidDecode:
    def test_no_args(self):
        pkt = build_wasm_packet(string_id=0, args=[])
        line, consumed = decode_wasm_packet(bytes(pkt), 0, TEST_DB)
        assert line is not None
        assert 'hello world' in line
        assert consumed == len(pkt)

    def test_int32_arg(self):
        pkt = build_wasm_packet(string_id=1, args=[(ARG_INT32, 42)])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert 'value=42' in line

    def test_string_arg(self):
        pkt = build_wasm_packet(string_id=2, args=[
            (ARG_STRING, "sensor"),
            (ARG_INT32, 7),
        ])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert 'name=sensor id=7' in line

    def test_int64_arg(self):
        pkt = build_wasm_packet(string_id=3, args=[(ARG_INT64, 9999999999)])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert '9999999999' in line

    def test_float64_arg(self):
        pkt = build_wasm_packet(string_id=4, args=[(ARG_FLOAT64, 3.14)])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert '3.14' in line

    def test_timestamp_in_output(self):
        pkt = build_wasm_packet(string_id=0, timestamp=12345, args=[])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        # Timestamp 12345ms = 00:00:12.345,000 in Zephyr format
        assert '00:00:12.345,000' in line

    def test_app_name_in_output(self):
        pkt = build_wasm_packet(string_id=0, args=[])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert 'test_app:' in line

    def test_level_names(self):
        for level, name in [(1, 'err'), (2, 'wrn'), (3, 'inf'), (4, 'dbg')]:
            pkt = build_wasm_packet(string_id=0, level=level, args=[])
            line, _ = decode_wasm_packet(pkt, 0, TEST_DB)
            assert f'<{name}>' in line


# ---------------------------------------------------------------------------
# Tests: Truncated packets
# ---------------------------------------------------------------------------

class TestTruncatedPackets:
    def test_truncated_header(self):
        # Only 10 bytes — less than 14-byte header
        pkt = build_wasm_packet(string_id=0, args=[])[:10]
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_truncated_int32_arg(self):
        # Full header + arg_count=1 + type byte, but missing value bytes
        pkt = build_wasm_packet(string_id=1, args=[(ARG_INT32, 42)])
        # Chop off last 2 bytes of the int32 value
        pkt = pkt[:-2]
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_truncated_int64_arg(self):
        pkt = build_wasm_packet(string_id=3, args=[(ARG_INT64, 999)])
        # Chop off last 4 bytes
        pkt = pkt[:-4]
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_truncated_float64_arg(self):
        pkt = build_wasm_packet(string_id=4, args=[(ARG_FLOAT64, 1.0)])
        pkt = pkt[:-4]
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_truncated_string_length(self):
        # String arg with only 1 byte of the 2-byte length field
        pkt = build_wasm_packet(string_id=5, args=[(ARG_STRING, "hello")])
        # Header(14) + arg_count(1) + type(1) + only 1 byte of len
        truncated = pkt[:14 + 1 + 1 + 1]
        line, consumed = decode_wasm_packet(truncated, 0, TEST_DB)
        assert line is None

    def test_truncated_string_data(self):
        # String with correct length field but data cut short
        pkt = build_wasm_packet(string_id=5, args=[(ARG_STRING, "hello world this is long")])
        # Chop off last 10 bytes of string data
        pkt = pkt[:-10]
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_long_string_valid(self):
        # A 200-byte string — within the 256 byte packet limit
        long_str = "A" * 200
        pkt = build_wasm_packet(string_id=5, args=[(ARG_STRING, long_str)])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is not None
        assert 'A' * 50 in line  # at least part of it decoded

    def test_missing_args(self):
        # Header says 2 args but only 1 provided
        pkt = build_wasm_packet(string_id=2, args=[
            (ARG_STRING, "x"),
            (ARG_INT32, 1),
        ])
        # Modify arg_count to claim 3 args
        pkt_mut = bytearray(pkt)
        pkt_mut[13] = 3  # claim 3 args but only 2 in data
        line, consumed = decode_wasm_packet(bytes(pkt_mut), 0, TEST_DB)
        assert line is None  # can't read the 3rd arg


# ---------------------------------------------------------------------------
# Tests: Unknown IDs and apps
# ---------------------------------------------------------------------------

class TestUnknownIds:
    def test_unknown_string_id(self):
        pkt = build_wasm_packet(string_id=999, args=[])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_unknown_app_id(self):
        pkt = build_wasm_packet(app_id=99, string_id=0, args=[])
        line, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert line is None

    def test_unknown_arg_type(self):
        # Inject an invalid arg type (0xFF)
        pkt = build_wasm_packet(string_id=1, args=[(ARG_INT32, 42)])
        pkt_mut = bytearray(pkt)
        pkt_mut[14] = 0xFF  # corrupt the arg type byte
        line, consumed = decode_wasm_packet(bytes(pkt_mut), 0, TEST_DB)
        assert line is None


# ---------------------------------------------------------------------------
# Tests: Offset handling (packet not at start of buffer)
# ---------------------------------------------------------------------------

class TestOffsetHandling:
    def test_packet_at_offset(self):
        # Pad with 10 garbage bytes before the packet
        padding = b'\x00' * 10
        pkt = build_wasm_packet(string_id=0, args=[])
        data = padding + pkt
        line, consumed = decode_wasm_packet(data, 10, TEST_DB)
        assert line is not None
        assert 'hello world' in line
        assert consumed == len(pkt)

    def test_multiple_packets_sequential(self):
        pkt1 = build_wasm_packet(string_id=0, timestamp=100, args=[])
        pkt2 = build_wasm_packet(string_id=1, timestamp=200, args=[(ARG_INT32, 5)])
        data = pkt1 + pkt2

        line1, consumed1 = decode_wasm_packet(data, 0, TEST_DB)
        assert line1 is not None
        assert '100' in line1

        line2, consumed2 = decode_wasm_packet(data, consumed1, TEST_DB)
        assert line2 is not None
        assert 'value=5' in line2


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    try:
        import pytest
        sys.exit(pytest.main([__file__, '-v']))
    except ImportError:
        import traceback
        test_classes = [v for k, v in globals().items()
                        if isinstance(v, type) and k.startswith('Test')]
        passed = 0
        failed = 0
        for cls in test_classes:
            instance = cls()
            methods = [m for m in dir(instance) if m.startswith('test_')]
            for method_name in sorted(methods):
                try:
                    getattr(instance, method_name)()
                    passed += 1
                    print(f"  PASS: {cls.__name__}.{method_name}")
                except AssertionError as e:
                    failed += 1
                    print(f"  FAIL: {cls.__name__}.{method_name}: {e}")
                except Exception as e:
                    failed += 1
                    print(f"  ERROR: {cls.__name__}.{method_name}: {e}")
                    traceback.print_exc()
        print(f"\n{passed} passed, {failed} failed, {passed + failed} total")
        sys.exit(1 if failed else 0)

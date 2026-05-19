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

from decode_wasm_log import decode_wasm_packet as _decode_wasm_packet


def decode_wasm_packet(data, offset, wasm_dbs, **kwargs):
    """Wrapper that returns (assembled_msg_or_None, bytes_consumed)."""
    result, consumed = _decode_wasm_packet(data, offset, wasm_dbs, **kwargs)
    if result is None:
        return None, consumed
    text, color, reset = result
    return f"{color}{text}{reset}", consumed


# Arg type constants (must match lib_wasm_dict_log.c)
ARG_INT32 = 0x01
ARG_INT64 = 0x02
ARG_FLOAT64 = 0x03
ARG_STRING = 0x04


def build_wasm_packet(app_id=0, level=3, string_id=0, args=None):
    """Build a valid WASM dict V2 packet for testing.

    V2 header (5 bytes): [app_id:1B][level:1B][string_id:2B LE][arg_count:1B]
    No msg_type marker, no timestamp.
    """
    pkt = bytearray()
    pkt.append(app_id)               # app_id
    pkt.append(level)                # log_level
    pkt += struct.pack('<H', string_id)  # string_id LE
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
        msg, consumed = decode_wasm_packet(bytes(pkt), 0, TEST_DB)
        assert msg is not None
        assert 'hello world' in msg
        assert consumed == len(pkt)

    def test_int32_arg(self):
        pkt = build_wasm_packet(string_id=1, args=[(ARG_INT32, 42)])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert 'value=42' in msg

    def test_string_arg(self):
        pkt = build_wasm_packet(string_id=2, args=[
            (ARG_STRING, "sensor"),
            (ARG_INT32, 7),
        ])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert 'name=sensor id=7' in msg

    def test_int64_arg(self):
        pkt = build_wasm_packet(string_id=3, args=[(ARG_INT64, 9999999999)])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert '9999999999' in msg

    def test_float64_arg(self):
        pkt = build_wasm_packet(string_id=4, args=[(ARG_FLOAT64, 3.14)])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert '3.14' in msg

    def test_no_timestamp_in_message(self):
        """V2 packets have no timestamp — returned message must not start with '['."""
        pkt = build_wasm_packet(string_id=0, args=[])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is not None
        # Strip any ANSI color prefix to check actual content
        import re
        stripped = re.sub(r'\x1b\[[0-9;]*m', '', msg)
        assert not stripped.startswith('[')

    def test_app_name_in_output(self):
        pkt = build_wasm_packet(string_id=0, args=[])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert 'test_app:' in msg

    def test_level_names(self):
        for level, name in [(1, 'err'), (2, 'wrn'), (3, 'inf'), (4, 'dbg')]:
            pkt = build_wasm_packet(string_id=0, level=level, args=[])
            msg, _ = decode_wasm_packet(pkt, 0, TEST_DB)
            assert f'<{name}>' in msg


# ---------------------------------------------------------------------------
# Tests: Truncated packets
# ---------------------------------------------------------------------------

class TestTruncatedPackets:
    def test_truncated_header(self):
        # Only 3 bytes — less than 5-byte V2 header
        pkt = build_wasm_packet(string_id=0, args=[])[:3]
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_truncated_int32_arg(self):
        # Full header + arg_count=1 + type byte, but missing value bytes
        pkt = build_wasm_packet(string_id=1, args=[(ARG_INT32, 42)])
        # Chop off last 2 bytes of the int32 value
        pkt = pkt[:-2]
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_truncated_int64_arg(self):
        pkt = build_wasm_packet(string_id=3, args=[(ARG_INT64, 999)])
        # Chop off last 4 bytes
        pkt = pkt[:-4]
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_truncated_float64_arg(self):
        pkt = build_wasm_packet(string_id=4, args=[(ARG_FLOAT64, 1.0)])
        pkt = pkt[:-4]
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_truncated_string_length(self):
        # String arg with only 1 byte of the 2-byte length field
        pkt = build_wasm_packet(string_id=5, args=[(ARG_STRING, "hello")])
        # Header(5) + type(1) + only 1 byte of len
        truncated = pkt[:5 + 1 + 1]
        msg, consumed = decode_wasm_packet(truncated, 0, TEST_DB)
        assert msg is None

    def test_truncated_string_data(self):
        # String with correct length field but data cut short
        pkt = build_wasm_packet(string_id=5, args=[(ARG_STRING, "hello world this is long")])
        # Chop off last 10 bytes of string data
        pkt = pkt[:-10]
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_long_string_valid(self):
        # A 200-byte string — within the 256 byte packet limit
        long_str = "A" * 200
        pkt = build_wasm_packet(string_id=5, args=[(ARG_STRING, long_str)])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is not None
        assert 'A' * 50 in msg  # at least part of it decoded

    def test_missing_args(self):
        # Header says 2 args but only 1 provided
        pkt = build_wasm_packet(string_id=2, args=[
            (ARG_STRING, "x"),
            (ARG_INT32, 1),
        ])
        # Modify arg_count to claim 3 args (arg_count is at offset 4 in V2)
        pkt_mut = bytearray(pkt)
        pkt_mut[4] = 3  # claim 3 args but only 2 in data
        msg, consumed = decode_wasm_packet(bytes(pkt_mut), 0, TEST_DB)
        assert msg is None  # can't read the 3rd arg


# ---------------------------------------------------------------------------
# Tests: Unknown IDs and apps
# ---------------------------------------------------------------------------

class TestUnknownIds:
    def test_unknown_string_id(self):
        pkt = build_wasm_packet(string_id=999, args=[])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_unknown_app_id(self):
        pkt = build_wasm_packet(app_id=99, string_id=0, args=[])
        msg, consumed = decode_wasm_packet(pkt, 0, TEST_DB)
        assert msg is None

    def test_unknown_arg_type(self):
        # Inject an invalid arg type (0xFF)
        pkt = build_wasm_packet(string_id=1, args=[(ARG_INT32, 42)])
        pkt_mut = bytearray(pkt)
        pkt_mut[5] = 0xFF  # corrupt the arg type byte (header is 5 bytes in V2)
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
        msg, consumed = decode_wasm_packet(data, 10, TEST_DB)
        assert msg is not None
        assert 'hello world' in msg
        assert consumed == len(pkt)

    def test_multiple_packets_sequential(self):
        pkt1 = build_wasm_packet(string_id=0, args=[])
        pkt2 = build_wasm_packet(string_id=1, args=[(ARG_INT32, 5)])
        data = pkt1 + pkt2

        msg1, consumed1 = decode_wasm_packet(data, 0, TEST_DB)
        assert msg1 is not None
        assert 'hello world' in msg1

        msg2, consumed2 = decode_wasm_packet(data, consumed1, TEST_DB)
        assert msg2 is not None
        assert 'value=5' in msg2


# ---------------------------------------------------------------------------
# Tests: Pointer types (%p, %s with various values)
# ---------------------------------------------------------------------------

# Extended dictionary with pointer format strings
POINTER_DB = {
    0: ({
        "0": {"fmt": "ptr=%p", "arg_types": ["int32"]},
        "1": {"fmt": "a=%p b=%p", "arg_types": ["int32", "int32"]},
        "2": {"fmt": "name=%s", "arg_types": ["string"]},
        "3": {"fmt": "ptr=%p val=%d", "arg_types": ["int32", "int32"]},
        "4": {"fmt": "100%% ptr=%p", "arg_types": ["int32"]},
        "5": {"fmt": "array[0]=%p array[1]=%p array[2]=%p",
              "arg_types": ["int32", "int32", "int32"]},
        "6": {"fmt": "deref: **pp=%p *p=%p val=%d",
              "arg_types": ["int32", "int32", "int32"]},
        "7": {"fmt": "null str=%s", "arg_types": ["string"]},
        "8": {"fmt": "fn_ptr=%p callback=%p", "arg_types": ["int32", "int32"]},
    }, "ptr_app"),
}


class TestPointerDecode:
    def test_basic_pointer(self):
        """Single %p prints as hex."""
        pkt = build_wasm_packet(string_id=0, args=[(ARG_INT32, 0x1000)],
                                 app_id=0)
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '0x1000' in line

    def test_pointer_deadbeef(self):
        """%p with 0xDEADBEEF value."""
        pkt = build_wasm_packet(string_id=0,
                                 args=[(ARG_INT32, 0xDEADBEEF - 2**32)])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        # signed int32 wraps — Python %#x on negative shows -0x...
        # The value on wire is a signed i32, but %p should show address
        assert 'ptr=' in line
        assert consumed == len(pkt)

    def test_null_pointer(self):
        """%p with NULL (0x0)."""
        pkt = build_wasm_packet(string_id=0, args=[(ARG_INT32, 0)])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert 'ptr=0' in line or 'ptr=0x0' in line

    def test_two_pointers(self):
        """Two %p in one format string."""
        pkt = build_wasm_packet(string_id=1, args=[
            (ARG_INT32, 0x2000),
            (ARG_INT32, 0x3000),
        ])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '0x2000' in line
        assert '0x3000' in line

    def test_pointer_mixed_with_int(self):
        """%p and %d in same format string."""
        pkt = build_wasm_packet(string_id=3, args=[
            (ARG_INT32, 0xCAFE),
            (ARG_INT32, 42),
        ])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '0xcafe' in line
        assert '42' in line

    def test_pointer_with_percent_literal(self):
        """%p after %% literal doesn't confuse decoder."""
        pkt = build_wasm_packet(string_id=4, args=[(ARG_INT32, 0xFF)])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '100%' in line
        assert '0xff' in line

    def test_array_of_pointers(self):
        """Three %p simulating an array of pointers."""
        pkt = build_wasm_packet(string_id=5, args=[
            (ARG_INT32, 0x10000),
            (ARG_INT32, 0x10004),
            (ARG_INT32, 0x10008),
        ])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '0x10000' in line
        assert '0x10004' in line
        assert '0x10008' in line

    def test_double_pointer_values(self):
        """Simulates **pp, *p, val — all are just int32 on wire."""
        pkt = build_wasm_packet(string_id=6, args=[
            (ARG_INT32, 0x4000),   # **pp (address of pointer-to-pointer)
            (ARG_INT32, 0x2000),   # *p (address of pointer)
            (ARG_INT32, 99),       # val (the dereferenced value)
        ])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '0x4000' in line
        assert '0x2000' in line
        assert '99' in line

    def test_function_pointer_values(self):
        """Function pointers are just addresses — same as any %p."""
        pkt = build_wasm_packet(string_id=8, args=[
            (ARG_INT32, 0x800),    # fn_ptr (WASM table index or address)
            (ARG_INT32, 0x804),    # callback
        ])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '0x800' in line
        assert '0x804' in line

    def test_empty_string(self):
        """%s with empty string — valid edge case."""
        pkt = build_wasm_packet(string_id=2, args=[(ARG_STRING, "")])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert 'name=' in line

    def test_null_string_as_empty(self):
        """%s with zero-length represents dereferenced NULL string."""
        pkt = build_wasm_packet(string_id=7, args=[(ARG_STRING, "")])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert 'null str=' in line

    def test_string_with_special_chars(self):
        """%s with path-like content (slashes, dots)."""
        pkt = build_wasm_packet(string_id=2,
                                 args=[(ARG_STRING, "/dev/sensor0")])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        assert '/dev/sensor0' in line

    def test_pointer_max_wasm32(self):
        """%p with max WASM32 address (just under 4GB)."""
        # WASM32 max address 0xFFFFFFFF as signed int32 = -1
        pkt = build_wasm_packet(string_id=0, args=[(ARG_INT32, -1)])
        line, consumed = decode_wasm_packet(pkt, 0, POINTER_DB)
        assert line is not None
        # Negative int32 formatted as %#x shows negative hex
        assert 'ptr=' in line
        assert consumed == len(pkt)


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

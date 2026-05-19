#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
Tests for stitch_wasm_dicts.py — merges per-app WASM dictionary JSONs
into a unified format.

Run: python3 -m pytest tests/test_stitch_tool.py -v
"""

import json
import os
import struct
import subprocess
import sys
import tempfile

import pytest

SCRIPT_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'scripts', 'stitch_wasm_dicts.py'
)


def run_stitch(args, cwd=None):
    """Helper to invoke stitch_wasm_dicts.py with given arguments."""
    cmd = [sys.executable, SCRIPT_PATH] + args
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    return result


def write_json(path, data):
    """Write a Python object as JSON to a file."""
    with open(path, 'w') as f:
        json.dump(data, f)


class TestStitchValid:
    """Tests for valid stitching operations."""

    def test_merge_two_apps(self, tmp_path):
        """Merge two app dictionaries into unified output."""
        sensor_dict = {"strings": {"0": "hello", "1": "world"}}
        network_dict = {"strings": {"0": "connect", "1": "send"}}

        sensor_path = str(tmp_path / "sensor.json")
        network_path = str(tmp_path / "network.json")
        output_path = str(tmp_path / "output.json")

        write_json(sensor_path, sensor_dict)
        write_json(network_path, network_dict)

        result = run_stitch([
            '--app', f'0:sensor_app:{sensor_path}',
            '--app', f'1:network_app:{network_path}',
            '-o', output_path
        ])

        assert result.returncode == 0, f"stderr: {result.stderr}"

        with open(output_path) as f:
            output = json.load(f)

        assert len(output) == 2
        assert output[0]["app_id"] == 0
        assert output[0]["app_name"] == "sensor_app"
        assert output[0]["dict"] == sensor_dict
        assert output[1]["app_id"] == 1
        assert output[1]["app_name"] == "network_app"
        assert output[1]["dict"] == network_dict

    def test_single_app(self, tmp_path):
        """Stitch a single app dictionary."""
        app_dict = {"formats": {"0": "Temperature: %d"}}
        app_path = str(tmp_path / "app.json")
        output_path = str(tmp_path / "output.json")

        write_json(app_path, app_dict)

        result = run_stitch([
            '--app', f'0:my_app:{app_path}',
            '-o', output_path
        ])

        assert result.returncode == 0, f"stderr: {result.stderr}"

        with open(output_path) as f:
            output = json.load(f)

        assert len(output) == 1
        assert output[0]["app_id"] == 0
        assert output[0]["app_name"] == "my_app"
        assert output[0]["dict"] == app_dict

    def test_three_apps(self, tmp_path):
        """Stitch three app dictionaries."""
        dicts = [
            {"strings": {"0": "alpha"}},
            {"strings": {"0": "beta"}},
            {"strings": {"0": "gamma"}},
        ]
        paths = []
        for i, d in enumerate(dicts):
            p = str(tmp_path / f"app{i}.json")
            write_json(p, d)
            paths.append(p)

        output_path = str(tmp_path / "output.json")

        result = run_stitch([
            '--app', f'0:app_alpha:{paths[0]}',
            '--app', f'1:app_beta:{paths[1]}',
            '--app', f'2:app_gamma:{paths[2]}',
            '-o', output_path
        ])

        assert result.returncode == 0, f"stderr: {result.stderr}"

        with open(output_path) as f:
            output = json.load(f)

        assert len(output) == 3
        for i, entry in enumerate(output):
            assert entry["app_id"] == i
            assert entry["dict"] == dicts[i]

    def test_output_sorted_by_app_id(self, tmp_path):
        """Output array is sorted by app_id regardless of input order."""
        dict_a = {"strings": {"0": "first"}}
        dict_b = {"strings": {"0": "second"}}

        path_a = str(tmp_path / "a.json")
        path_b = str(tmp_path / "b.json")
        output_path = str(tmp_path / "output.json")

        write_json(path_a, dict_a)
        write_json(path_b, dict_b)

        # Pass app_id=5 first, app_id=2 second
        result = run_stitch([
            '--app', f'5:app_b:{path_b}',
            '--app', f'2:app_a:{path_a}',
            '-o', output_path
        ])

        assert result.returncode == 0, f"stderr: {result.stderr}"

        with open(output_path) as f:
            output = json.load(f)

        assert output[0]["app_id"] == 2
        assert output[0]["app_name"] == "app_a"
        assert output[1]["app_id"] == 5
        assert output[1]["app_name"] == "app_b"


class TestStitchErrors:
    """Tests for error handling in the stitch tool."""

    def test_duplicate_app_id_error(self, tmp_path):
        """Duplicate app_id should cause an error."""
        dict_a = {"strings": {"0": "a"}}
        dict_b = {"strings": {"0": "b"}}

        path_a = str(tmp_path / "a.json")
        path_b = str(tmp_path / "b.json")
        output_path = str(tmp_path / "output.json")

        write_json(path_a, dict_a)
        write_json(path_b, dict_b)

        result = run_stitch([
            '--app', f'0:app_a:{path_a}',
            '--app', f'0:app_b:{path_b}',
            '-o', output_path
        ])

        assert result.returncode != 0
        assert "duplicate" in result.stderr.lower() or "app_id" in result.stderr.lower()

    def test_missing_file_error(self, tmp_path):
        """Non-existent dictionary file should cause an error."""
        output_path = str(tmp_path / "output.json")
        missing_path = str(tmp_path / "nonexistent.json")

        result = run_stitch([
            '--app', f'0:my_app:{missing_path}',
            '-o', output_path
        ])

        assert result.returncode != 0
        assert "not found" in result.stderr.lower() or "no such file" in result.stderr.lower() or "missing" in result.stderr.lower()

    def test_invalid_json_error(self, tmp_path):
        """Invalid JSON content should cause an error."""
        bad_path = str(tmp_path / "bad.json")
        output_path = str(tmp_path / "output.json")

        with open(bad_path, 'w') as f:
            f.write("{this is not valid json!!!")

        result = run_stitch([
            '--app', f'0:my_app:{bad_path}',
            '-o', output_path
        ])

        assert result.returncode != 0
        assert "json" in result.stderr.lower() or "parse" in result.stderr.lower()

    def test_invalid_app_arg_format(self, tmp_path):
        """Argument with only 2 colon-separated parts (missing app_name) should error."""
        some_path = str(tmp_path / "x.json")
        output_path = str(tmp_path / "output.json")

        write_json(some_path, {"strings": {}})

        # Only "0:path" — missing app_name
        result = run_stitch([
            '--app', f'0:{some_path}',
            '-o', output_path
        ])

        assert result.returncode != 0
        assert "format" in result.stderr.lower() or "invalid" in result.stderr.lower()

    def test_non_integer_app_id(self, tmp_path):
        """Non-integer app_id should cause an error."""
        some_path = str(tmp_path / "x.json")
        output_path = str(tmp_path / "output.json")

        write_json(some_path, {"strings": {}})

        result = run_stitch([
            '--app', f'abc:my_app:{some_path}',
            '-o', output_path
        ])

        assert result.returncode != 0
        assert "integer" in result.stderr.lower() or "invalid" in result.stderr.lower()


# ---------------------------------------------------------------------------
# TestUnifiedJsonLoading — tests for parse_wasm_dbs() with unified JSON format
# ---------------------------------------------------------------------------

# Add scripts/ to path so we can import decode_wasm_log
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts'))
from decode_wasm_log import parse_wasm_dbs  # noqa: E402
from decode_wasm_log import decode_wasm_packet as _decode_wasm_packet  # noqa: E402


def decode_wasm_packet(data, offset, wasm_dbs, **kwargs):
    """Wrapper that assembles the tuple return into a single string."""
    result, consumed = _decode_wasm_packet(data, offset, wasm_dbs, **kwargs)
    if result is None:
        return None, consumed
    text, color, reset = result
    return f"{color}{text}{reset}", consumed


def build_wasm_packet(app_id=0, level=3, string_id=0, args=None):
    """Build a V2 WASM dictionary log packet (binary)."""
    pkt = bytearray()
    pkt.append(app_id)
    pkt.append(level)
    pkt += struct.pack('<H', string_id)
    if args is None:
        args = []
    pkt.append(len(args))
    for atype, value in args:
        pkt.append(atype)
        if atype == 0x01:  # INT32
            pkt += struct.pack('<i', value)
        elif atype == 0x02:  # INT64
            pkt += struct.pack('<q', value)
        elif atype == 0x03:  # FLOAT64
            pkt += struct.pack('<d', value)
        elif atype == 0x04:  # STRING
            encoded = value.encode('utf-8')
            pkt += struct.pack('<H', len(encoded))
            pkt += encoded
    return bytes(pkt)


class TestUnifiedJsonLoading:
    """Tests for parse_wasm_dbs() loading unified JSON format."""

    def test_load_unified_format(self, tmp_path):
        """Load a unified JSON and verify returned dict has correct app_ids and names."""
        unified = [
            {
                "app_id": 0,
                "app_name": "sensor_app",
                "dict": {"0": {"fmt": "hello %d", "arg_types": ["int32"]}}
            },
            {
                "app_id": 1,
                "app_name": "network_app",
                "dict": {"0": {"fmt": "port=%d", "arg_types": ["int32"]}}
            },
        ]
        json_path = str(tmp_path / "unified.json")
        write_json(json_path, unified)

        dbs = parse_wasm_dbs(json_path)

        assert 0 in dbs
        assert 1 in dbs
        assert dbs[0][1] == "sensor_app"
        assert dbs[1][1] == "network_app"
        assert dbs[0][0]["0"]["fmt"] == "hello %d"
        assert dbs[1][0]["0"]["fmt"] == "port=%d"

    def test_route_by_app_id(self, tmp_path):
        """Build V2 packets with different app_ids and decode them with loaded dbs."""
        unified = [
            {
                "app_id": 0,
                "app_name": "sensor_app",
                "dict": {"0": {"fmt": "temp=%d", "arg_types": ["int32"]}}
            },
            {
                "app_id": 1,
                "app_name": "network_app",
                "dict": {"0": {"fmt": "port=%d", "arg_types": ["int32"]}}
            },
        ]
        json_path = str(tmp_path / "unified.json")
        write_json(json_path, unified)

        dbs = parse_wasm_dbs(json_path)

        # Packet for app_id=0
        pkt0 = build_wasm_packet(app_id=0, level=3, string_id=0, args=[(0x01, 42)])
        msg0, consumed0 = decode_wasm_packet(pkt0, 0, dbs, use_color=False)
        assert msg0 is not None
        assert "sensor_app" in msg0
        assert "temp=42" in msg0

        # Packet for app_id=1
        pkt1 = build_wasm_packet(app_id=1, level=3, string_id=0, args=[(0x01, 8080)])
        msg1, consumed1 = decode_wasm_packet(pkt1, 0, dbs, use_color=False)
        assert msg1 is not None
        assert "network_app" in msg1
        assert "port=8080" in msg1

    def test_missing_app_id_in_unified(self, tmp_path):
        """Packet with app_id not in JSON returns None."""
        unified = [
            {
                "app_id": 0,
                "app_name": "sensor_app",
                "dict": {"0": {"fmt": "hello %d", "arg_types": ["int32"]}}
            },
        ]
        json_path = str(tmp_path / "unified.json")
        write_json(json_path, unified)

        dbs = parse_wasm_dbs(json_path)

        # Packet with app_id=5 which is not in the unified JSON
        pkt = build_wasm_packet(app_id=5, level=3, string_id=0, args=[(0x01, 1)])
        msg, consumed = decode_wasm_packet(pkt, 0, dbs, use_color=False)
        assert msg is None

    def test_empty_unified_json(self, tmp_path):
        """Empty array [] returns empty dict."""
        json_path = str(tmp_path / "unified.json")
        write_json(json_path, [])

        dbs = parse_wasm_dbs(json_path)

        assert dbs == {}


# ---------------------------------------------------------------------------
# TestEndToEndIntegration — full pipeline: stitch -> decode
# ---------------------------------------------------------------------------

import shutil


class TestEndToEndIntegration:
    """Integration: stitch -> decode produces correct output."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_stitch_then_decode_roundtrip(self):
        """Stitch per-app JSONs, load unified, decode V2 packets."""
        sensor_dict = {
            "0": {"fmt": "temp=%d C", "arg_types": ["int32"]},
            "1": {"fmt": "humidity=%d%%", "arg_types": ["int32"]},
        }
        network_dict = {
            "0": {"fmt": "connected to %s port %d", "arg_types": ["string", "int32"]},
        }

        sensor_path = os.path.join(self.tmpdir, 'sensor_dict.json')
        network_path = os.path.join(self.tmpdir, 'network_dict.json')
        write_json(sensor_path, sensor_dict)
        write_json(network_path, network_dict)

        # Stitch
        unified_path = os.path.join(self.tmpdir, 'unified.json')
        result = run_stitch([
            '--app', f'0:sensor_app:{sensor_path}',
            '--app', f'1:network_app:{network_path}',
            '-o', unified_path,
        ])
        assert result.returncode == 0, f"stitch failed: {result.stderr}"

        # Load unified
        dbs = parse_wasm_dbs(unified_path)
        assert 0 in dbs
        assert 1 in dbs

        # Decode sensor packet (app_id=0, level=3=INF, string_id=0, 1 arg: int32=25)
        pkt_sensor = build_wasm_packet(app_id=0, level=3, string_id=0, args=[(0x01, 25)])
        msg, _ = decode_wasm_packet(pkt_sensor, 0, dbs, use_color=False)
        assert msg is not None
        assert 'sensor_app' in msg
        assert 'temp=25 C' in msg

        # Decode network packet (app_id=1, level=3, string_id=0, 2 args: string + int32)
        pkt_network = build_wasm_packet(app_id=1, level=3, string_id=0, args=[
            (0x04, "example.com"),
            (0x01, 443),
        ])
        msg, _ = decode_wasm_packet(pkt_network, 0, dbs, use_color=False)
        assert msg is not None
        assert 'network_app' in msg
        assert 'connected to example.com port 443' in msg

    def test_pointer_format_in_unified_decode(self):
        """Decode %p format from unified dictionary."""
        unified = [{
            "app_id": 0,
            "app_name": "ptr_test",
            "dict": {"0": {"fmt": "buf=%p len=%d", "arg_types": ["int32", "int32"]}},
        }]
        path = os.path.join(self.tmpdir, 'unified.json')
        write_json(path, unified)

        dbs = parse_wasm_dbs(path)
        pkt = build_wasm_packet(app_id=0, level=3, string_id=0, args=[
            (0x01, 0x20001000),
            (0x01, 256),
        ])
        msg, _ = decode_wasm_packet(pkt, 0, dbs, use_color=False)
        assert msg is not None
        assert '0x20001000' in msg
        assert '256' in msg

    def test_multi_type_args_roundtrip(self):
        """All arg types decode correctly through unified pipeline."""
        unified = [{
            "app_id": 0,
            "app_name": "types_app",
            "dict": {"0": {"fmt": "i=%d l=%d f=%f s=%s", "arg_types": ["int32", "int64", "float64", "string"]}},
        }]
        path = os.path.join(self.tmpdir, 'unified.json')
        write_json(path, unified)

        dbs = parse_wasm_dbs(path)
        pkt = build_wasm_packet(app_id=0, level=3, string_id=0, args=[
            (0x01, -42),        # int32
            (0x02, 9876543210), # int64
            (0x03, 3.14),       # float64
            (0x04, "hello"),    # string
        ])
        msg, _ = decode_wasm_packet(pkt, 0, dbs, use_color=False)
        assert msg is not None
        assert 'i=-42' in msg
        assert '9876543210' in msg
        assert '3.14' in msg
        assert 's=hello' in msg

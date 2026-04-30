"""Unit tests for struct layout comparison (no external tools needed)."""
import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from compare import compare_layouts, suggest_fix


class TestSuggestFix:
    def test_enum_size_mismatch(self):
        wasm = {"offset": 28, "size": 4, "type": "device_status", "is_struct": False}
        native = {"offset": 28, "size": 1, "type": "device_status", "is_struct": False}
        fix = suggest_fix("status", wasm, native)
        assert "enum" in fix
        assert "uint32_t" in fix

    def test_alignment_mismatch(self):
        wasm = {"offset": 8, "size": 8, "type": "uint64_t", "is_struct": False}
        native = {"offset": 4, "size": 8, "type": "uint64_t", "is_struct": False}
        fix = suggest_fix("serial", wasm, native)
        assert "aligned(8)" in fix

    def test_cascade_offset_shift(self):
        wasm = {"offset": 32, "size": 1, "type": "uint8_t", "is_struct": False}
        native = {"offset": 29, "size": 1, "type": "uint8_t", "is_struct": False}
        fix = suggest_fix("channel", wasm, native)
        assert "caused by earlier" in fix

    def test_nested_struct_size(self):
        wasm = {"offset": 8, "size": 16, "type": "device_info", "is_struct": True}
        native = {"offset": 4, "size": 12, "type": "device_info", "is_struct": True}
        fix = suggest_fix("info", wasm, native)
        assert "inner struct" in fix

    def test_no_mismatch(self):
        wasm = {"offset": 0, "size": 4, "type": "uint32_t", "is_struct": False}
        native = {"offset": 0, "size": 4, "type": "uint32_t", "is_struct": False}
        fix = suggest_fix("field", wasm, native)
        assert fix is None


class TestCompareLayouts:
    def test_matching_structs(self):
        layout = {
            "size": 8,
            "members": [
                {"name": "x", "offset": 0, "size": 4, "type": "uint32_t", "is_struct": False},
                {"name": "y", "offset": 4, "size": 4, "type": "float", "is_struct": False},
            ]
        }
        count, suggestions, nested = compare_layouts("test", layout, layout)
        assert count == 0
        assert suggestions == []
        assert nested == set()

    def test_offset_mismatch(self):
        native = {
            "size": 12,
            "members": [
                {"name": "a", "offset": 0, "size": 1, "type": "uint8_t", "is_struct": False},
                {"name": "b", "offset": 4, "size": 8, "type": "uint64_t", "is_struct": False},
            ]
        }
        wasm = {
            "size": 16,
            "members": [
                {"name": "a", "offset": 0, "size": 1, "type": "uint8_t", "is_struct": False},
                {"name": "b", "offset": 8, "size": 8, "type": "uint64_t", "is_struct": False},
            ]
        }
        count, suggestions, nested = compare_layouts("test", native, wasm)
        assert count >= 1
        assert any("aligned" in s for s in suggestions)

    def test_nested_struct_detected(self):
        native = {
            "size": 8,
            "members": [
                {"name": "inner", "offset": 0, "size": 8, "type": "inner_t", "is_struct": True},
            ]
        }
        wasm = {
            "size": 8,
            "members": [
                {"name": "inner", "offset": 0, "size": 8, "type": "inner_t", "is_struct": True},
            ]
        }
        count, suggestions, nested = compare_layouts("outer", native, wasm)
        assert "inner_t" in nested

    def test_sizeof_mismatch(self):
        native = {
            "size": 40,
            "members": [
                {"name": "x", "offset": 0, "size": 1, "type": "uint8_t", "is_struct": False},
            ]
        }
        wasm = {
            "size": 48,
            "members": [
                {"name": "x", "offset": 0, "size": 1, "type": "uint8_t", "is_struct": False},
            ]
        }
        count, _, _ = compare_layouts("test", native, wasm)
        assert count >= 1

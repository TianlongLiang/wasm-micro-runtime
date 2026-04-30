"""Unit tests for NativeSymbol discovery (no external tools needed)."""
import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from discovery import (
    parse_native_symbols,
    signature_pointer_indices,
    find_struct_types,
)


class TestParseNativeSymbols:
    def test_single_entry(self):
        src = '{ "foo", foo_native, "(*~)i", NULL }'
        result = parse_native_symbols(src)
        assert len(result) == 1
        assert result[0]["export_name"] == "foo"
        assert result[0]["func_name"] == "foo_native"
        assert result[0]["signature"] == "(*~)i"

    def test_multiple_entries(self):
        src = """
        static NativeSymbol native_symbols[] = {
            { "process_report", process_report_native, "(*~)i", NULL },
            { "configure_device", configure_device_native, "(*~)i", NULL },
            { "print_int", print_int_native, "(i)", NULL },
        };
        """
        result = parse_native_symbols(src)
        assert len(result) == 3
        assert result[0]["export_name"] == "process_report"
        assert result[2]["signature"] == "(i)"

    def test_no_symbols(self):
        assert parse_native_symbols("int main() {}") == []


class TestSignaturePointerIndices:
    def test_pointer_and_size(self):
        assert signature_pointer_indices("(*~)i") == [0]

    def test_two_pointers(self):
        assert signature_pointer_indices("(**)")  == [0, 1]

    def test_no_pointers(self):
        assert signature_pointer_indices("(ii)i") == []

    def test_mixed(self):
        assert signature_pointer_indices("(i*~i*)i") == [1, 4]

    def test_empty(self):
        assert signature_pointer_indices("()") == []

    def test_invalid(self):
        assert signature_pointer_indices("bad") == []


class TestFindStructTypes:
    SAMPLE_SOURCE = """
    static int
    process_report_native(wasm_exec_env_t exec_env,
                          struct sensor_report *rpt, int size)
    { return 0; }

    static int
    process_raw_native(wasm_exec_env_t exec_env,
                       void *buf, int size)
    { return 0; }
    """

    def test_struct_pointer(self):
        types, unchecked = find_struct_types(self.SAMPLE_SOURCE,
                                             "process_report_native", [0])
        assert types == ["sensor_report"]
        assert unchecked == []

    def test_void_pointer(self):
        types, unchecked = find_struct_types(self.SAMPLE_SOURCE,
                                             "process_raw_native", [0])
        assert types == []
        assert len(unchecked) == 1
        assert unchecked[0]["func_name"] == "process_raw_native"
        assert "void" in unchecked[0]["param_type"]

    def test_function_not_found(self):
        types, unchecked = find_struct_types(self.SAMPLE_SOURCE,
                                             "nonexistent_func", [0])
        assert types == []
        assert unchecked == []

    def test_comments_stripped(self):
        src = """
        /* static int fake(wasm_exec_env_t e, struct fake_struct *p) {} */
        static int
        real_func(wasm_exec_env_t exec_env, struct real_struct *p)
        { return 0; }
        """
        types, _ = find_struct_types(src, "real_func", [0])
        assert types == ["real_struct"]

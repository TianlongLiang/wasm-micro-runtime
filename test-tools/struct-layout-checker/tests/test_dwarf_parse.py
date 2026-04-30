"""Unit tests for DWARF output parsing (no external tools needed)."""
import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from dwarf import parse_llvm_dwarf_output


SAMPLE_LLVM_OUTPUT = """\
0x0000000b:   DW_TAG_compile_unit
                DW_AT_language\t(DW_LANG_C11)

0x00000030:   DW_TAG_base_type
                DW_AT_name\t("unsigned int")
                DW_AT_byte_size\t(0x04)

0x00000040:   DW_TAG_base_type
                DW_AT_name\t("unsigned char")
                DW_AT_byte_size\t(0x01)

0x00000050:   DW_TAG_typedef
                DW_AT_name\t("uint32_t")
                DW_AT_type\t(0x00000030)

0x00000060:   DW_TAG_typedef
                DW_AT_name\t("uint8_t")
                DW_AT_type\t(0x00000040)

0x00000070:   DW_TAG_structure_type
                DW_AT_name\t("test_struct")
                DW_AT_byte_size\t(0x08)

0x00000080:     DW_TAG_member
                  DW_AT_name\t("id")
                  DW_AT_type\t(0x00000060 "uint8_t")
                  DW_AT_data_member_location\t(0x00)

0x00000090:     DW_TAG_member
                  DW_AT_name\t("value")
                  DW_AT_type\t(0x00000050 "uint32_t")
                  DW_AT_data_member_location\t(0x04)

0x000000a0:   DW_TAG_variable
                DW_AT_name\t("__probe_test_struct")
"""


class TestParseLlvmDwarfOutput:
    def test_basic_struct(self):
        layouts = parse_llvm_dwarf_output(SAMPLE_LLVM_OUTPUT, None)
        assert "test_struct" in layouts
        s = layouts["test_struct"]
        assert s["size"] == 8
        assert len(s["members"]) == 2
        assert s["members"][0]["name"] == "id"
        assert s["members"][0]["offset"] == 0
        assert s["members"][0]["size"] == 1
        assert s["members"][1]["name"] == "value"
        assert s["members"][1]["offset"] == 4
        assert s["members"][1]["size"] == 4

    def test_filtered_by_wanted(self):
        layouts = parse_llvm_dwarf_output(SAMPLE_LLVM_OUTPUT,
                                          {"test_struct"})
        assert "test_struct" in layouts

    def test_filtered_out(self):
        layouts = parse_llvm_dwarf_output(SAMPLE_LLVM_OUTPUT,
                                          {"nonexistent"})
        assert "test_struct" not in layouts

    def test_empty_output(self):
        layouts = parse_llvm_dwarf_output("", None)
        assert layouts == {}


NESTED_LLVM_OUTPUT = """\
0x00000030:   DW_TAG_base_type
                DW_AT_name\t("unsigned char")
                DW_AT_byte_size\t(0x01)

0x00000050:   DW_TAG_structure_type
                DW_AT_name\t("inner")
                DW_AT_byte_size\t(0x04)

0x00000060:     DW_TAG_member
                  DW_AT_name\t("x")
                  DW_AT_type\t(0x00000030 "unsigned char")
                  DW_AT_data_member_location\t(0x00)

0x00000070:   DW_TAG_structure_type
                DW_AT_name\t("outer")
                DW_AT_byte_size\t(0x08)

0x00000080:     DW_TAG_member
                  DW_AT_name\t("nested")
                  DW_AT_type\t(0x00000050 "inner")
                  DW_AT_data_member_location\t(0x00)

0x00000090:   DW_TAG_variable
"""


class TestNestedStructDetection:
    def test_nested_is_struct(self):
        layouts = parse_llvm_dwarf_output(NESTED_LLVM_OUTPUT, None)
        assert "outer" in layouts
        member = layouts["outer"]["members"][0]
        assert member["name"] == "nested"
        assert member["is_struct"] is True
        assert member["size"] == 4

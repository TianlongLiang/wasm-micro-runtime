#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
Unit tests for extract_log_strings.py using C source fixture files.

Run from the dictionary-log directory:
    python3 -m pytest tests/test_extract_log_strings.py -v
or:
    python3 tests/test_extract_log_strings.py
"""

import os
import sys

# Add scripts/ to path so we can import the extraction module
SCRIPT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts')
sys.path.insert(0, SCRIPT_DIR)

TEST_DIR = os.path.dirname(os.path.abspath(__file__))

from extract_log_strings import (
    extract_log_calls,
    transform_source,
    classify_specifier,
    build_type_descriptor,
    TYPE_INT32,
    TYPE_INT64,
    TYPE_FLOAT64,
    TYPE_STRING,
)


def load_fixture(filename):
    """Load a C test fixture file."""
    path = os.path.join(TEST_DIR, filename)
    with open(path, 'r') as f:
        return f.read()


# ---------------------------------------------------------------------------
# Tests using valid_basic.c
# ---------------------------------------------------------------------------

class TestValidBasic:
    def setup_method(self):
        self.source = load_fixture('valid_basic.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_all_calls_extracted(self):
        assert len(self.calls) == 5

    def test_no_skipped(self):
        assert len(self.skipped) == 0

    def test_no_args_call(self):
        call = self.calls[0]
        assert call['fmt'] == 'hello world'
        assert call['arg_types'] == []
        assert call['type_descriptor'] == 0x0

    def test_single_int_arg(self):
        call = self.calls[1]
        assert call['fmt'] == 'error code %d'
        assert call['arg_types'] == [TYPE_INT32]
        assert call['level'] == 'ERR'

    def test_two_uint_args(self):
        call = self.calls[2]
        assert call['fmt'] == 'count %u limit %u'
        assert call['arg_types'] == [TYPE_INT32, TYPE_INT32]
        assert call['level'] == 'WRN'

    def test_hex_format(self):
        call = self.calls[3]
        assert call['fmt'] == 'hex value 0x%x'
        assert call['arg_types'] == [TYPE_INT32]

    def test_three_args(self):
        call = self.calls[4]
        assert call['fmt'] == 'verbose %d %u %d'
        assert call['arg_types'] == [TYPE_INT32, TYPE_INT32, TYPE_INT32]
        assert call['level'] == 'VERBOSE'

    def test_levels_correct(self):
        levels = [c['level'] for c in self.calls]
        assert levels == ['INF', 'ERR', 'WRN', 'DBG', 'VERBOSE']


# ---------------------------------------------------------------------------
# Tests using valid_multiline.c
# ---------------------------------------------------------------------------

class TestValidMultiline:
    def setup_method(self):
        self.source = load_fixture('valid_multiline.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_all_calls_extracted(self):
        assert len(self.calls) == 4

    def test_no_skipped(self):
        assert len(self.skipped) == 0

    def test_multiline_format_string(self):
        call = self.calls[0]
        assert 'sensor %d reading: value=%u offset=%d applied' == call['fmt']
        assert call['arg_types'] == [TYPE_INT32, TYPE_INT32, TYPE_INT32]

    def test_string_concatenation(self):
        call = self.calls[1]
        assert call['fmt'] == 'string concat works'
        assert call['arg_types'] == []

    def test_nested_parens_in_args(self):
        call = self.calls[2]
        assert call['fmt'] == 'nested parens: val=%d'
        assert call['arg_types'] == [TYPE_INT32]
        assert '(int32_t)(value + offset)' in call['args_text']

    def test_expression_in_args(self):
        call = self.calls[3]
        assert call['fmt'] == 'function arg: result=%d'
        assert 'sensor_id * 2' in call['args_text']


# ---------------------------------------------------------------------------
# Tests using valid_types.c
# ---------------------------------------------------------------------------

class TestValidTypes:
    def setup_method(self):
        self.source = load_fixture('valid_types.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_all_calls_extracted(self):
        assert len(self.calls) == 6

    def test_no_skipped(self):
        assert len(self.skipped) == 0

    def test_int32_specifiers(self):
        # %d %i %u %x %X %o %c %p — all are int32 on wasm32
        call = self.calls[0]
        assert call['arg_types'] == [TYPE_INT32] * 8
        assert len(call['arg_types']) == 8

    def test_int64_specifiers(self):
        # %ld %llu %llx
        call = self.calls[1]
        assert call['arg_types'] == [TYPE_INT64, TYPE_INT64, TYPE_INT64]

    def test_float64_specifiers(self):
        # %f %e %g %F %E %G
        call = self.calls[2]
        assert call['arg_types'] == [TYPE_FLOAT64] * 6

    def test_string_specifier(self):
        call = self.calls[3]
        assert call['arg_types'] == [TYPE_STRING]

    def test_percent_literal_not_counted(self):
        # "100%% complete, %d items" — %% is not an arg
        call = self.calls[4]
        assert call['arg_types'] == [TYPE_INT32]
        assert '%%' in call['fmt']

    def test_width_precision_modifiers(self):
        # %10d %-20s %08x %5.2f
        call = self.calls[5]
        assert call['arg_types'] == [TYPE_INT32, TYPE_STRING, TYPE_INT32, TYPE_FLOAT64]


# ---------------------------------------------------------------------------
# Tests using invalid_pri_macros.c
# ---------------------------------------------------------------------------

class TestInvalidPriMacros:
    def setup_method(self):
        self.source = load_fixture('invalid_pri_macros.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_pri_calls_skipped(self):
        assert len(self.skipped) == 3  # PRIu32, PRId64+PRIu32, PRIx32

    def test_valid_call_still_extracted(self):
        assert len(self.calls) == 1
        assert self.calls[0]['fmt'] == 'after PRI: works fine %d'

    def test_skipped_positions_recorded(self):
        for start, end, reason in self.skipped:
            assert 'PRI' in reason
            assert start < end


# ---------------------------------------------------------------------------
# Tests using invalid_non_literal.c
# ---------------------------------------------------------------------------

class TestInvalidNonLiteral:
    def setup_method(self):
        self.source = load_fixture('invalid_non_literal.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_non_literal_calls_skipped(self):
        assert len(self.skipped) == 3  # variable, macro, empty

    def test_valid_call_after_invalid(self):
        assert len(self.calls) == 1
        assert self.calls[0]['fmt'] == 'recovery after errors: %d'

    def test_transform_marks_skipped(self):
        transformed = transform_source(self.source, self.calls, self.skipped)
        assert '/* WASM_LOG_DICT: skipped' in transformed
        # The valid call should be transformed
        assert 'wasm_log_dict(' in transformed


# ---------------------------------------------------------------------------
# Tests using invalid_too_many_args.c
# ---------------------------------------------------------------------------

class TestInvalidTooManyArgs:
    def setup_method(self):
        self.source = load_fixture('invalid_too_many_args.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_9_args_skipped(self):
        assert len(self.skipped) == 1
        assert 'too many' in self.skipped[0][2]

    def test_8_args_ok(self):
        # Find the call with 8 %d specifiers
        eight_arg_call = [c for c in self.calls if len(c['arg_types']) == 8]
        assert len(eight_arg_call) == 1

    def test_7_args_ok(self):
        seven_arg_call = [c for c in self.calls if len(c['arg_types']) == 7]
        assert len(seven_arg_call) == 1

    def test_valid_count(self):
        assert len(self.calls) == 2  # 8-arg and 7-arg calls


# ---------------------------------------------------------------------------
# Tests using valid_strings.c
# ---------------------------------------------------------------------------

class TestValidStrings:
    def setup_method(self):
        self.source = load_fixture('valid_strings.c')
        self.calls, self.skipped = extract_log_calls(self.source)

    def test_all_calls_extracted(self):
        assert len(self.calls) == 6

    def test_no_skipped(self):
        assert len(self.skipped) == 0

    def test_single_string_arg(self):
        call = self.calls[0]
        assert call['fmt'] == 'device: %s'
        assert call['arg_types'] == [TYPE_STRING]

    def test_string_mixed_with_ints(self):
        call = self.calls[1]
        assert call['fmt'] == 'device %s status=%d port=%u'
        assert call['arg_types'] == [TYPE_STRING, TYPE_INT32, TYPE_INT32]

    def test_multiple_strings(self):
        call = self.calls[2]
        assert call['fmt'] == 'src=%s dst=%s'
        assert call['arg_types'] == [TYPE_STRING, TYPE_STRING]

    def test_string_with_width(self):
        call = self.calls[3]
        assert call['fmt'] == 'name=%-20s id=%d'
        assert call['arg_types'] == [TYPE_STRING, TYPE_INT32]

    def test_long_format_with_string(self):
        call = self.calls[4]
        assert '%s' in call['fmt']
        assert call['arg_types'] == [TYPE_STRING]

    def test_string_in_middle(self):
        call = self.calls[5]
        assert call['fmt'] == "value='%s' end"
        assert call['arg_types'] == [TYPE_STRING]


# ---------------------------------------------------------------------------
# Tests for classify_specifier (unit level)
# ---------------------------------------------------------------------------

class TestClassifySpecifier:
    def test_percent_percent(self):
        assert classify_specifier('%%') is None

    def test_int32_types(self):
        for spec in ['%d', '%i', '%u', '%x', '%X', '%o', '%c', '%p']:
            assert classify_specifier(spec) == TYPE_INT32, f"Failed for {spec}"

    def test_int64_types(self):
        for spec in ['%ld', '%lu', '%lx', '%lld', '%llu', '%llx']:
            assert classify_specifier(spec) == TYPE_INT64, f"Failed for {spec}"

    def test_float64_types(self):
        for spec in ['%f', '%e', '%g', '%F', '%E', '%G']:
            assert classify_specifier(spec) == TYPE_FLOAT64, f"Failed for {spec}"

    def test_string(self):
        assert classify_specifier('%s') == TYPE_STRING

    def test_with_width(self):
        assert classify_specifier('%10d') == TYPE_INT32
        assert classify_specifier('%-20s') == TYPE_STRING
        assert classify_specifier('%08x') == TYPE_INT32
        assert classify_specifier('%5.2f') == TYPE_FLOAT64


# ---------------------------------------------------------------------------
# Tests for build_type_descriptor (unit level)
# ---------------------------------------------------------------------------

class TestBuildTypeDescriptor:
    def test_empty(self):
        assert build_type_descriptor([]) == 0x0

    def test_one_int(self):
        assert build_type_descriptor([TYPE_INT32]) == 0x1

    def test_two_args(self):
        assert build_type_descriptor([TYPE_INT32, TYPE_FLOAT64]) == 0x31

    def test_max_8(self):
        assert build_type_descriptor([TYPE_INT32] * 8) == 0x11111111

    def test_truncates_beyond_8(self):
        # Only first 8 are packed
        assert build_type_descriptor([TYPE_INT32] * 10) == 0x11111111


# ---------------------------------------------------------------------------
# Tests for transform_source
# ---------------------------------------------------------------------------

class TestTransformSource:
    def test_adds_define(self):
        source = 'LOG_INF("hello");'
        calls, skipped = extract_log_calls(source)
        transformed = transform_source(source, calls, skipped)
        assert transformed.startswith('#define WASM_LOG_DICT 1\n')

    def test_replaces_log_call(self):
        source = 'LOG_INF("val=%d", x);'
        calls, skipped = extract_log_calls(source)
        transformed = transform_source(source, calls, skipped)
        assert 'wasm_log_dict(WASM_LOG_LEVEL_INF, /*id=*/0, /*types=*/0x1, x)' in transformed

    def test_skipped_becomes_comment(self):
        source = 'LOG_INF(variable, x);'
        calls, skipped = extract_log_calls(source)
        transformed = transform_source(source, calls, skipped)
        assert '/* WASM_LOG_DICT: skipped' in transformed

    def test_preserves_surrounding_code(self):
        source = load_fixture('valid_basic.c')
        calls, skipped = extract_log_calls(source)
        transformed = transform_source(source, calls, skipped)
        assert '#include "wasm_log.h"' in transformed
        assert 'void test_valid_basic(void)' in transformed
        assert 'int32_t i = 42;' in transformed


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
            for method_name in methods:
                if hasattr(instance, 'setup_method'):
                    instance.setup_method()
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

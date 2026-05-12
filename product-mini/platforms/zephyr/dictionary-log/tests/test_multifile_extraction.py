#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
Tests for preprocessor-based multi-file extraction.

Run: python3 -m pytest tests/test_multifile_extraction.py -v
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

import pytest

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPT_DIR = os.path.join(TEST_DIR, '..', 'scripts')
SCRIPT = os.path.join(SCRIPT_DIR, 'extract_log_strings.py')
MULTIFILE_DIR = os.path.join(TEST_DIR, 'multifile')
SINGLEFILE_DIR = os.path.join(TEST_DIR, 'singlefile')
ERROR_DIR = os.path.join(TEST_DIR, 'error_cases')

# Find wasi-sdk clang
CLANG = None
for candidate in ['/opt/wasi-sdk/bin/clang',
                  '/opt/wasi-sdk-29.0-x86_64-linux/bin/clang',
                  '/opt/wasi-sdk-25.0-x86_64-linux/bin/clang']:
    if os.path.exists(candidate):
        CLANG = candidate
        break

pytestmark = pytest.mark.skipif(
    CLANG is None, reason="wasi-sdk clang not found"
)


def run_extract(sources, include_dirs=None, defines=None, output_dir=None,
                json_path=None, expect_fail=False):
    """Run the extraction script and return (returncode, stdout, stderr, outdir, jsonpath)."""
    if output_dir is None:
        output_dir = tempfile.mkdtemp()
    if json_path is None:
        json_path = os.path.join(output_dir, 'dict.json')

    cmd = [
        sys.executable, SCRIPT,
        '--clang', CLANG,
        '--target', 'wasm32-wasi',
        '-o-dir', output_dir,
        '-j', json_path,
    ]
    for inc in (include_dirs or []):
        cmd.extend(['-I', inc])
    for d in (defines or ['CUR_LOG_LEVEL=4']):
        cmd.extend(['-D', d])
    cmd.extend(sources)

    result = subprocess.run(cmd, capture_output=True, text=True)

    if not expect_fail:
        assert result.returncode == 0, (
            f"Script failed (rc={result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )

    return result.returncode, result.stdout, result.stderr, output_dir, json_path


class TestMultiFileExtraction:
    """Tests using the multifile/ test fixtures (3 .c + 2 .h)."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()
        self.sources = [
            os.path.join(MULTIFILE_DIR, 'main.c'),
            os.path.join(MULTIFILE_DIR, 'sensor.c'),
            os.path.join(MULTIFILE_DIR, 'helper.c'),
        ]

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_extracts_from_all_files(self):
        rc, stdout, stderr, _, json_path = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        assert len(d) > 0
        # Script reports per-file results to stderr
        assert 'main.c' in stderr
        assert 'sensor.c' in stderr
        assert 'helper.c' in stderr

    def test_flat_id_space_sequential(self):
        rc, _, _, _, json_path = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        ids = sorted(int(k) for k in d.keys())
        assert ids == list(range(len(ids)))

    def test_header_inline_function_extracted(self):
        rc, _, _, _, json_path = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        fmts = [v['fmt'] for v in d.values()]
        # util.h has "util: module %d initialized"
        assert any('util: module' in fmt for fmt in fmts)

    def test_pri_macro_resolved(self):
        rc, _, _, _, json_path = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        fmts = [v['fmt'] for v in d.values()]
        # common.h uses PRIu32 — should be resolved (no "PRI" in output)
        assert any('memory: used=' in fmt for fmt in fmts)
        assert not any('PRI' in fmt for fmt in fmts)

    def test_output_files_created_per_input(self):
        rc, _, _, outdir, _ = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        assert os.path.isfile(os.path.join(outdir, 'main_dict.i'))
        assert os.path.isfile(os.path.join(outdir, 'sensor_dict.i'))
        assert os.path.isfile(os.path.join(outdir, 'helper_dict.i'))

    def test_transformed_files_contain_wasm_log_dict(self):
        rc, _, _, outdir, _ = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        for name in ['main_dict.i', 'sensor_dict.i', 'helper_dict.i']:
            path = os.path.join(outdir, name)
            content = open(path).read()
            assert 'wasm_log_dict(' in content

    def test_no_untransformed_wasm_log_with_string(self):
        rc, _, _, outdir, _ = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        for name in ['main_dict.i', 'sensor_dict.i', 'helper_dict.i']:
            path = os.path.join(outdir, name)
            content = open(path).read()
            remaining = re.findall(r'\bwasm_log\s*\(\s*\d+\s*,\s*"', content)
            assert len(remaining) == 0, f"Untransformed wasm_log in {name}: {remaining[:3]}"

    def test_shared_header_log_in_multiple_files(self):
        """common.h's log_memory_usage is included by sensor.c and helper.c.
        Each gets its own ID (separate call sites)."""
        rc, _, _, _, json_path = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        fmts = [v['fmt'] for v in d.values()]
        # "memory: used=%u total=%u" should appear multiple times (separate IDs)
        memory_fmts = [f for f in fmts if 'memory: used=' in f]
        # main.c, sensor.c, and helper.c all call log_memory_usage
        assert len(memory_fmts) >= 2

    def test_source_file_tracked_in_json(self):
        rc, _, _, _, json_path = run_extract(
            self.sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        # At least some entries should have source_file info
        source_files = set(v.get('source_file', '') for v in d.values())
        assert len(source_files) > 0


class TestSingleFile:
    """Basic single-file sanity tests."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_single_file_works(self):
        sources = [os.path.join(MULTIFILE_DIR, 'main.c')]
        rc, stdout, stderr, _, json_path = run_extract(
            sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        assert 'main.c' in stderr
        with open(json_path) as f:
            d = json.load(f)
        assert len(d) > 0

    def test_output_file_named_correctly(self):
        sources = [os.path.join(MULTIFILE_DIR, 'main.c')]
        rc, _, _, outdir, _ = run_extract(
            sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        assert os.path.isfile(os.path.join(outdir, 'main_dict.i'))


class TestDryRunFailure:
    """Tests that dry-run compile catches errors before extraction."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_syntax_error_caught(self):
        sources = [os.path.join(ERROR_DIR, 'syntax_error.c')]
        rc, stdout, stderr, _, _ = run_extract(
            sources,
            include_dirs=[ERROR_DIR],
            output_dir=self.tmpdir,
            expect_fail=True,
        )
        assert rc != 0
        assert 'error' in stderr.lower() or 'error' in stdout.lower()

    def test_missing_include_caught(self):
        sources = [os.path.join(ERROR_DIR, 'missing_include.c')]
        rc, stdout, stderr, _, _ = run_extract(
            sources,
            include_dirs=[ERROR_DIR],
            output_dir=self.tmpdir,
            expect_fail=True,
        )
        assert rc != 0
        assert 'nonexistent' in stderr.lower() or 'not found' in stderr.lower()

    def test_no_log_calls_reports_zero(self):
        sources = [os.path.join(ERROR_DIR, 'no_log_calls.c')]
        rc, stdout, stderr, _, _ = run_extract(
            sources,
            include_dirs=[ERROR_DIR],
            output_dir=self.tmpdir,
            expect_fail=True,
        )
        # Script exits non-zero when no log calls are found
        assert rc != 0
        assert 'no wasm_log' in stderr.lower()


class TestIdAssignment:
    """Test that IDs are assigned sequentially across files."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_ids_dont_restart_per_file(self):
        """When processing multiple files, IDs continue from previous file."""
        sources = [
            os.path.join(MULTIFILE_DIR, 'main.c'),
            os.path.join(MULTIFILE_DIR, 'sensor.c'),
        ]
        rc, _, _, _, json_path = run_extract(
            sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)

        # Get IDs grouped by source file
        ids_by_file = {}
        for k, v in d.items():
            sf = v.get('source_file', 'unknown')
            ids_by_file.setdefault(sf, []).append(int(k))

        # Verify no ID collisions
        all_ids = sorted(int(k) for k in d.keys())
        assert len(all_ids) == len(set(all_ids)), "Duplicate IDs found!"

    def test_order_matches_input_order(self):
        """First file's IDs come before second file's IDs."""
        sources = [
            os.path.join(MULTIFILE_DIR, 'main.c'),
            os.path.join(MULTIFILE_DIR, 'sensor.c'),
        ]
        rc, _, _, outdir, json_path = run_extract(
            sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )

        # Check that main_dict.i uses lower IDs than sensor_dict.i
        main_content = open(os.path.join(outdir, 'main_dict.i')).read()
        sensor_content = open(os.path.join(outdir, 'sensor_dict.i')).read()

        main_ids = [int(m) for m in re.findall(r'wasm_log_dict\(\d+,\s*(\d+),', main_content)]
        sensor_ids = [int(m) for m in re.findall(r'wasm_log_dict\(\d+,\s*(\d+),', sensor_content)]

        if main_ids and sensor_ids:
            assert max(main_ids) < min(sensor_ids), (
                f"main IDs {main_ids} should all be less than sensor IDs {sensor_ids}"
            )

    def test_same_string_different_files_different_ids(self):
        """Same format string in two files gets different IDs."""
        # sensor.c and helper.c both have "sensor: read complete"
        sources = [
            os.path.join(MULTIFILE_DIR, 'sensor.c'),
            os.path.join(MULTIFILE_DIR, 'helper.c'),
        ]
        rc, _, _, _, json_path = run_extract(
            sources,
            include_dirs=[MULTIFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(json_path) as f:
            d = json.load(f)
        # Find entries with "sensor: read complete"
        matching = [k for k, v in d.items() if 'sensor: read complete' in v['fmt']]
        assert len(matching) >= 2, (
            f"Expected 2+ entries for duplicate string, got {len(matching)}"
        )
        # They should have different IDs
        assert len(set(matching)) == len(matching)

    def test_file_order_affects_ids(self):
        """Swapping input file order changes ID assignment."""
        sources_ab = [
            os.path.join(MULTIFILE_DIR, 'main.c'),
            os.path.join(MULTIFILE_DIR, 'sensor.c'),
        ]
        sources_ba = [
            os.path.join(MULTIFILE_DIR, 'sensor.c'),
            os.path.join(MULTIFILE_DIR, 'main.c'),
        ]

        tmpdir_ab = tempfile.mkdtemp()
        tmpdir_ba = tempfile.mkdtemp()

        try:
            _, _, _, _, json_ab = run_extract(
                sources_ab, include_dirs=[MULTIFILE_DIR], output_dir=tmpdir_ab)
            _, _, _, _, json_ba = run_extract(
                sources_ba, include_dirs=[MULTIFILE_DIR], output_dir=tmpdir_ba)

            with open(json_ab) as f:
                d_ab = json.load(f)
            with open(json_ba) as f:
                d_ba = json.load(f)

            # Same total count
            assert len(d_ab) == len(d_ba)
            # But first entry's fmt should differ (different file processed first)
            assert d_ab['0']['fmt'] != d_ba['0']['fmt']
        finally:
            shutil.rmtree(tmpdir_ab, ignore_errors=True)
            shutil.rmtree(tmpdir_ba, ignore_errors=True)


class TestFormatTypeClassification:
    """Tests using types_test.c to verify all format specifiers are classified correctly."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()
        sources = [os.path.join(SINGLEFILE_DIR, 'types_test.c')]
        rc, _, _, _, self.json_path = run_extract(
            sources,
            include_dirs=[SINGLEFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(self.json_path) as f:
            self.d = json.load(f)
        self.fmts = {v['fmt']: v for v in self.d.values()}

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_int32_specifiers(self):
        """d, i, u, x, X, o should all be int32."""
        entry = self.fmts.get('int types: d=%d i=%i u=%u x=0x%x X=0x%X o=%o')
        assert entry is not None, f"Format not found. Available: {list(self.fmts.keys())[:5]}"
        assert entry['arg_types'] == ['int32'] * 6

    def test_char_and_pointer(self):
        """%c and %p are int32."""
        entry = self.fmts.get('char and ptr: c=%c p=%p')
        assert entry is not None
        assert entry['arg_types'] == ['int32', 'int32']

    def test_int64_specifiers(self):
        """%ld, %llu, %llx should be int64."""
        entry = self.fmts.get('long types: ld=%ld llu=%llu llx=0x%llx')
        assert entry is not None
        assert entry['arg_types'] == ['int64'] * 3

    def test_float64_specifiers(self):
        """%f, %e, %g should be float64."""
        entry = self.fmts.get('float types: f=%f e=%e g=%g')
        assert entry is not None
        assert entry['arg_types'] == ['float64'] * 3

    def test_float64_uppercase(self):
        """%F, %E, %G should be float64."""
        entry = self.fmts.get('FLOAT types: F=%F E=%E G=%G')
        assert entry is not None
        assert entry['arg_types'] == ['float64'] * 3

    def test_string_specifier(self):
        """%s should be string."""
        entry = self.fmts.get('string: name=%s')
        assert entry is not None
        assert entry['arg_types'] == ['string']

    def test_multiple_strings(self):
        """Multiple %s in one call."""
        entry = self.fmts.get('multi string: a=%s b=%s')
        assert entry is not None
        assert entry['arg_types'] == ['string', 'string']

    def test_width_precision_modifiers(self):
        """%10d, %-20s, %08x, %5.2f — modifiers don't change type."""
        entry = self.fmts.get('width: %10d %-20s %08x %5.2f')
        assert entry is not None
        assert entry['arg_types'] == ['int32', 'string', 'int32', 'float64']

    def test_percent_literal_not_counted(self):
        """%% is not an argument."""
        entry = self.fmts.get('progress: 100%% done, %d items processed')
        assert entry is not None
        assert entry['arg_types'] == ['int32']

    def test_pri_macros_resolved(self):
        """PRIu32 and PRId64 resolved to correct types by preprocessor."""
        # After preprocessing, "u32=%" PRIu32 " i64=%" PRId64
        # becomes "u32=%u i64=%lld" or similar
        matching = [v for v in self.d.values() if 'PRI:' in v['fmt']]
        assert len(matching) == 1, f"Expected 1 PRI entry, got {len(matching)}"
        entry = matching[0]
        # PRIu32 → int32, PRId64 → int64
        assert entry['arg_types'] == ['int32', 'int64']

    def test_mixed_types(self):
        """int + string + float + int in one call."""
        entry = self.fmts.get('mixed: int=%d str=%s float=%f hex=0x%x')
        assert entry is not None
        assert entry['arg_types'] == ['int32', 'string', 'float64', 'int32']

    def test_zero_args(self):
        """No format specifiers = empty arg_types."""
        entry = self.fmts.get('no args at all')
        assert entry is not None
        assert entry['arg_types'] == []

    def test_max_eight_args(self):
        """Exactly 8 args should work."""
        entry = self.fmts.get('eight: %d %d %d %d %d %d %d %d')
        assert entry is not None
        assert entry['arg_types'] == ['int32'] * 8

    def test_total_calls_extracted(self):
        """All LOG calls in types_test.c should be extracted."""
        # 19 LOG calls in types_test.c (14 original + 5 new edge cases)
        assert len(self.d) >= 19

    def test_escaped_quotes(self):
        """Escaped quotes inside format string are preserved."""
        matching = [v for v in self.d.values() if 'say' in v['fmt'] and 'hello' in v['fmt']]
        assert len(matching) == 1
        entry = matching[0]
        assert '\\"hello\\"' in entry['fmt'] or '"hello"' in entry['fmt']
        assert entry['arg_types'] == ['string']

    def test_backslash_sequences(self):
        """Literal \\n in format string (not a newline)."""
        matching = [v for v in self.d.values() if 'path:' in v['fmt'] and 'line:' in v['fmt']]
        assert len(matching) == 1
        entry = matching[0]
        assert entry['arg_types'] == ['string', 'int32']

    def test_very_long_format_string(self):
        """Format strings >100 chars are extracted without truncation."""
        matching = [v for v in self.d.values() if 'very long format string' in v['fmt']]
        assert len(matching) == 1
        entry = matching[0]
        assert len(entry['fmt']) > 100
        assert 'end' in entry['fmt']
        assert entry['arg_types'] == ['int32']

    def test_adjacent_calls_same_line(self):
        """Two LOG calls on the same line both get extracted."""
        fmts = [v['fmt'] for v in self.d.values()]
        assert 'adjacent_a' in fmts
        assert 'adjacent_b' in fmts


class TestEdgeCases:
    """Tests using edge_cases.c for conditional compilation and robustness."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()
        sources = [os.path.join(SINGLEFILE_DIR, 'edge_cases.c')]
        rc, _, _, _, self.json_path = run_extract(
            sources,
            include_dirs=[SINGLEFILE_DIR],
            output_dir=self.tmpdir,
        )
        with open(self.json_path) as f:
            self.d = json.load(f)
        self.fmts = [v['fmt'] for v in self.d.values()]

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_if_zero_block_not_extracted(self):
        """LOG inside #if 0 should NOT appear after preprocessing."""
        assert not any('should never appear' in fmt for fmt in self.fmts)

    def test_variable_fmt_skipped(self):
        """wasm_log with variable format string is silently skipped."""
        assert not any('runtime string' in fmt for fmt in self.fmts)

    def test_valid_after_skip_extracted(self):
        """Valid LOG call after a skipped one is still extracted."""
        assert any('valid after variable fmt' in fmt for fmt in self.fmts)

    def test_function_with_no_logs_no_problem(self):
        """A function with no LOG calls doesn't cause errors."""
        # Just verify extraction succeeded (setup_method didn't fail)
        assert len(self.d) >= 2

    def test_comment_before_log_ok(self):
        """Comments before LOG calls don't interfere."""
        assert any('after comment' in fmt for fmt in self.fmts)

    def test_wasm_log_text_in_format_string(self):
        """'wasm_log(' text inside a format string doesn't confuse extraction."""
        # This call has "wasm_log(%d)" literally inside the format string
        matching = [v for v in self.d.values()
                    if 'calling wasm_log' in v['fmt']]
        assert len(matching) == 1
        entry = matching[0]
        assert entry['arg_types'] == ['int32', 'int32']

    def test_wasm_log_description_in_string(self):
        """A format string describing the wasm_log API doesn't cause issues."""
        matching = [v for v in self.d.values()
                    if 'wasm_log(level, fmt, ...)' in v['fmt']]
        assert len(matching) == 1
        assert matching[0]['arg_types'] == []

    def test_valid_after_wasm_log_in_string(self):
        """Valid call following a tricky format string is still extracted."""
        matching = [v for v in self.d.values()
                    if 'valid after wasm_log-in-string' in v['fmt']]
        assert len(matching) == 1
        assert matching[0]['arg_types'] == ['int32']

    def test_total_valid_calls(self):
        """Only the valid LOG calls are extracted, not dead code or variable fmt."""
        # edge_cases.c has: 1 in #if 0 (excluded), 1 variable (skipped),
        # 1 "valid after variable fmt", 1 "after comment",
        # 3 wasm_log-in-string tests = 5 valid
        assert len(self.d) == 5


class TestEmptyAndMinimal:
    """Tests for empty/minimal source files."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_empty_file(self):
        """A file with only a comment produces zero entries."""
        sources = [os.path.join(ERROR_DIR, 'empty_file.c')]
        rc, _, stderr, _, _ = run_extract(
            sources,
            include_dirs=[ERROR_DIR],
            output_dir=self.tmpdir,
            expect_fail=True,
        )
        # Should exit non-zero (no log calls found)
        assert rc != 0

    def test_no_log_calls_file(self):
        """Valid C with no wasm_log calls produces zero entries."""
        sources = [os.path.join(ERROR_DIR, 'no_log_calls.c')]
        rc, _, stderr, _, _ = run_extract(
            sources,
            include_dirs=[ERROR_DIR],
            output_dir=self.tmpdir,
            expect_fail=True,
        )
        assert rc != 0

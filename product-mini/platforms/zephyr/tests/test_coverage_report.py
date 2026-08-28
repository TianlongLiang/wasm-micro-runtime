# Copyright (C) 2026 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).parents[1] / "coverage_report.py"
SPEC = importlib.util.spec_from_file_location("coverage_report", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ValidateTraceTest(unittest.TestCase):
    def write_trace(self, directory, contents, name="coverage.json"):
        trace = Path(directory) / name
        trace.write_text(contents)
        return trace

    def test_missing_trace_names_the_path(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "missing.json"

            with self.assertRaisesRegex(ValueError, str(trace)):
                MODULE.validate_trace(trace, Path(directory))

    def test_malformed_trace_names_the_path(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(directory, "not json")

            with self.assertRaisesRegex(ValueError, str(trace)):
                MODULE.validate_trace(trace, Path(directory))

    def test_trace_without_zephyr_production_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                json.dumps({"files": [{"file": "core/foo.c", "lines": []}]}),
            )

            with self.assertRaisesRegex(ValueError, str(trace)):
                MODULE.validate_trace(trace, Path(directory))

    def test_trace_with_zephyr_production_file_returns_resolved_path(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                json.dumps(
                    {
                        "files": [
                            {
                                "file": "core/shared/platform/zephyr/zephyr_time.c",
                                "lines": [],
                            }
                        ]
                    }
                ),
            )

            self.assertEqual(
                MODULE.validate_trace(trace, Path(directory)), trace.resolve()
            )


class GcovrBaseCommandTest(unittest.TestCase):
    def test_builds_platform_filtered_command_in_trace_order(self):
        root = Path("/checkout")
        traces = [Path("/traces/first.json"), Path("/traces/second.json")]

        self.assertEqual(
            MODULE.gcovr_base_command(root, traces),
            [
                "gcovr",
                "-r",
                str(root),
                "--filter",
                "core/shared/platform/zephyr/",
                "--add-tracefile",
                str(traces[0]),
                "--add-tracefile",
                str(traces[1]),
            ],
        )


class AggregateCoverageTest(unittest.TestCase):
    def write_trace(self, directory, name="coverage.json"):
        trace = Path(directory) / name
        trace.write_text(
            json.dumps(
                {
                    "files": [
                        {
                            "file": "core/shared/platform/zephyr/zephyr_time.c",
                            "lines": [],
                        }
                    ]
                }
            )
        )
        return trace

    def test_generates_all_reports_and_publishes_inputs_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "checkout"
            root.mkdir()
            input_dir = Path(directory) / "twister-input"
            input_dir.mkdir()
            first = self.write_trace(input_dir, "first.json")
            second = self.write_trace(input_dir, "second.json")
            output = Path(directory) / "aggregate"
            output.mkdir()
            (output / "stale.txt").write_text("stale")
            temporary = output.with_name(f"{output.name}.tmp")

            with mock.patch.object(MODULE.subprocess, "run") as run:
                self.assertTrue(
                    MODULE.aggregate_coverage(root, [first, second], output)
                )

            base = MODULE.gcovr_base_command(root, [first.resolve(), second.resolve()])
            self.assertEqual(
                run.call_args_list,
                [
                    mock.call(
                        base + ["--html-details", str(temporary / "index.html")],
                        check=True,
                        cwd=root,
                    ),
                    mock.call(
                        base
                        + [
                            "--xml-pretty",
                            "--xml",
                            str(temporary / "coverage.xml"),
                        ],
                        check=True,
                        cwd=root,
                    ),
                    mock.call(
                        base
                        + [
                            "--json-pretty",
                            "--json",
                            str(temporary / "coverage.json"),
                        ],
                        check=True,
                        cwd=root,
                    ),
                    mock.call(
                        base
                        + [
                            "--txt-metric",
                            "line",
                            "--txt",
                            str(temporary / "line-summary.txt"),
                        ],
                        check=True,
                        cwd=root,
                    ),
                    mock.call(
                        base
                        + [
                            "--txt-metric",
                            "branch",
                            "--txt",
                            str(temporary / "branch-summary.txt"),
                        ],
                        check=True,
                        cwd=root,
                    ),
                ],
            )
            self.assertEqual(
                (output / "inputs.txt").read_text(),
                f"{first.resolve()}\n{second.resolve()}\n",
            )
            self.assertFalse((output / "stale.txt").exists())
            self.assertTrue(first.exists())
            self.assertFalse(temporary.exists())

    def test_gcovr_failure_leaves_existing_aggregate_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "checkout"
            root.mkdir()
            trace = self.write_trace(directory)
            output = Path(directory) / "aggregate"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("keep")

            with mock.patch.object(
                MODULE.subprocess,
                "run",
                side_effect=subprocess.CalledProcessError(1, ["gcovr"]),
            ):
                with self.assertRaises(subprocess.CalledProcessError):
                    MODULE.aggregate_coverage(root, [trace], output)

            self.assertEqual(sentinel.read_text(), "keep")

    def test_invalid_trace_does_not_run_gcovr(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = Path(directory) / "invalid.json"
            trace.write_text("not json")

            with mock.patch.object(MODULE.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    MODULE.aggregate_coverage(root, [trace], root / "aggregate")

            run.assert_not_called()

    def test_duplicate_resolved_traces_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.write_trace(directory)

            with mock.patch.object(MODULE.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "duplicate"):
                    MODULE.aggregate_coverage(root, [trace, trace], root / "aggregate")

            run.assert_not_called()

    def test_refuses_to_replace_an_input_artifact_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_dir = root / "twister-input"
            input_dir.mkdir()
            trace = self.write_trace(input_dir)

            with mock.patch.object(MODULE.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "input artifact"):
                    MODULE.aggregate_coverage(root, [trace], input_dir)

            self.assertTrue(trace.exists())
            run.assert_not_called()

    def test_refuses_stale_temporary_directory_containing_input_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "aggregate"
            temporary = output.with_name(f"{output.name}.tmp")
            temporary.mkdir()
            trace = self.write_trace(temporary)

            with mock.patch.object(MODULE.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "input artifact"):
                    MODULE.aggregate_coverage(root, [trace], output)

            self.assertTrue(trace.exists())
            run.assert_not_called()

    def test_refuses_output_symlink_without_touching_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.write_trace(root)
            target = root / "target"
            target.mkdir()
            sentinel = target / "keep.txt"
            sentinel.write_text("keep")
            output = root / "aggregate"
            output.symlink_to(target, target_is_directory=True)

            with mock.patch.object(MODULE.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "symlink"):
                    MODULE.aggregate_coverage(root, [trace], output)

            self.assertEqual(sentinel.read_text(), "keep")
            self.assertTrue(output.is_symlink())
            run.assert_not_called()

    def test_publish_rename_failure_restores_existing_aggregate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.write_trace(root)
            output = root / "aggregate"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("keep")
            original_replace = Path.replace

            def fail_new_publication(path, target):
                if path.name == "aggregate.tmp" and Path(target) == output:
                    raise OSError("simulated publication failure")
                return original_replace(path, target)

            with (
                mock.patch.object(MODULE.subprocess, "run"),
                mock.patch.object(
                    Path, "replace", autospec=True, side_effect=fail_new_publication
                ),
            ):
                with self.assertRaisesRegex(OSError, "simulated publication failure"):
                    MODULE.aggregate_coverage(root, [trace], output)

            self.assertEqual(sentinel.read_text(), "keep")
            self.assertFalse((root / "aggregate.tmp").exists())
            self.assertFalse((root / "aggregate.bak").exists())


class CoverageReportCliTest(unittest.TestCase):
    def test_cli_forwards_root_output_and_trace_arguments(self):
        root = Path("/checkout")
        output = Path("/reports")
        trace = Path("/traces/coverage.json")

        with (
            mock.patch.object(MODULE, "aggregate_coverage", return_value=True) as run,
            mock.patch.object(
                sys,
                "argv",
                [
                    str(SCRIPT),
                    "--root",
                    str(root),
                    "--output-dir",
                    str(output),
                    str(trace),
                ],
            ),
        ):
            self.assertEqual(MODULE.main(), 0)

        run.assert_called_once_with(root, [trace], output)


if __name__ == "__main__":
    unittest.main()

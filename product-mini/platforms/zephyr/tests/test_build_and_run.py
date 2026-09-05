# Copyright (C) 2026 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import importlib.util
import io
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).parents[1] / "build_and_run.py"
SPEC = importlib.util.spec_from_file_location("build_and_run", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ResolveTestRootTest(unittest.TestCase):
    def test_help_describes_sample_and_test_roots(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--help"],
            check=True,
            capture_output=True,
            text=True,
        )

        self.assertIn("sample or tests/... test root", result.stdout)
        self.assertIn("sample.yaml or testcase.yaml", result.stdout)
        self.assertIn("--coverage", result.stdout)
        self.assertIn("--scenario", result.stdout)

    def test_resolves_existing_sample(self):
        self.assertEqual(MODULE.resolve_test_root("simple"), Path("simple"))

    def test_resolves_nested_test_application(self):
        self.assertEqual(
            MODULE.resolve_test_root("tests/platform_api"),
            Path("tests/platform_api"),
        )

    def test_rejects_parent_traversal(self):
        with self.assertRaises(ValueError):
            MODULE.resolve_test_root("../simple")

    def test_rejects_symlink_root_that_resolves_outside_platform_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            platform_dir = Path(directory) / "zephyr"
            platform_dir.mkdir()
            outside = Path(directory) / "outside"
            outside.mkdir()
            (outside / "sample.yaml").write_text("tests: {}\n")
            (platform_dir / "escaped").symlink_to(
                outside, target_is_directory=True
            )

            with (
                mock.patch.object(MODULE, "HERE", platform_dir),
                self.assertRaisesRegex(ValueError, "must stay below"),
            ):
                MODULE.resolve_test_root("escaped")

    def test_main_reports_symlink_loop_without_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            platform_dir = Path(directory) / "zephyr"
            platform_dir.mkdir()
            (platform_dir / "loop").symlink_to("loop", target_is_directory=True)
            log_dir = Path(directory) / "logs"

            with (
                mock.patch.object(MODULE, "HERE", platform_dir),
                mock.patch.object(MODULE, "LOG_DIR", log_dir),
                mock.patch.object(sys, "argv", [str(SCRIPT), "loop"]),
                mock.patch.object(
                    sys, "stderr", new_callable=io.StringIO
                ) as stderr,
                self.assertRaises(SystemExit) as error,
            ):
                MODULE.main()

            self.assertEqual(error.exception.code, 2)
            self.assertIn("Symlink loop", stderr.getvalue())
            self.assertNotIn("Traceback", stderr.getvalue())


class ScenarioNamingTest(unittest.TestCase):
    def test_scenario_slug_uses_readable_artifact_component(self):
        self.assertEqual(
            MODULE.scenario_slug("wamr.zephyr.platform_api.kernel_stack_info"),
            "wamr-zephyr-platform-api-kernel-stack-info",
        )

    def test_artifact_name_includes_scenario_slug(self):
        self.assertEqual(
            MODULE.artifact_name(
                Path("tests/platform_api"),
                "native_sim",
                "wamr.zephyr.platform_api.kernel",
            ),
            "tests-platform_api-native_sim-wamr-zephyr-platform-api-kernel-af729dc6",
        )

    def test_scenarios_with_same_readable_slug_have_distinct_artifacts(self):
        scenarios = ("wamr.foo_bar", "wamr.foo.bar", "wamr.foo-bar")
        artifacts = {
            MODULE.artifact_name(Path("tests/platform_api"), "native_sim", name)
            for name in scenarios
        }

        self.assertEqual(len(artifacts), len(scenarios))
        self.assertEqual(
            artifacts,
            {
                "tests-platform_api-native_sim-wamr-foo-bar-c30eabbb",
                "tests-platform_api-native_sim-wamr-foo-bar-5c71e64b",
                "tests-platform_api-native_sim-wamr-foo-bar-39d93039",
            },
        )

    def test_scenarios_without_alphanumeric_content_are_rejected(self):
        for scenario in ("", ".", "_", "-", "...", "_-.-"):
            with self.subTest(scenario=scenario), self.assertRaises(ValueError):
                MODULE.scenario_slug(scenario)

    def test_scenario_with_path_separator_is_rejected(self):
        with self.assertRaises(ValueError):
            MODULE.artifact_name(
                Path("tests/platform_api"), "native_sim", "wamr/zephyr"
            )


class TwisterCommandTest(unittest.TestCase):
    def test_selected_scenario_is_forwarded_exactly(self):
        scenario = "wamr.zephyr.platform_api.kernel_stack_info"
        command = MODULE.twister_command(
            Path("tests/platform_api"),
            "native_sim",
            False,
            scenario=scenario,
        )

        tokens = shlex.split(command)
        self.assertEqual(tokens[tokens.index("-s") + 1], scenario)

    def test_coverage_forwards_zephyr_3_7_gcovr_options(self):
        command = MODULE.twister_command(
            Path("tests/platform_api"), "native_sim", False, coverage=True
        )

        self.assertIn("--coverage", command.split())
        self.assertIn(f"--coverage-basedir {MODULE.WAMR_ROOT}", command)
        self.assertIn("--coverage-tool gcovr", command)
        self.assertIn("--coverage-formats html,xml", command)

    def test_no_docker_paths_remain_single_shell_arguments(self):
        checkout = Path("/tmp/WAMR checkout's coverage")
        with mock.patch.object(MODULE, "WAMR_ROOT", checkout):
            command = MODULE.twister_command(
                Path("tests/platform api"),
                "native_sim",
                False,
                coverage=True,
            )

        tokens = shlex.split(command)
        module_dir = str(checkout)
        platform_dir = f"{module_dir}/product-mini/platforms/zephyr"
        self.assertEqual(
            tokens[tokens.index("-T") + 1], f"{platform_dir}/tests/platform api"
        )
        self.assertIn(f"EXTRA_ZEPHYR_MODULES={module_dir}", tokens)
        self.assertEqual(
            tokens[tokens.index("--outdir") + 1],
            f"{platform_dir}/build/twister-tests-platform api-native_sim-coverage",
        )
        self.assertEqual(
            tokens[tokens.index("--coverage-basedir") + 1], module_dir
        )

    def test_docker_coverage_uses_container_paths(self):
        command = MODULE.twister_command(
            Path("tests/platform_api"), "native_sim", True, coverage=True
        )
        tokens = shlex.split(command)

        self.assertEqual(
            tokens[tokens.index("--coverage-basedir") + 1], MODULE.MODULE_DIR
        )
        self.assertEqual(
            tokens[tokens.index("--outdir") + 1],
            f"{MODULE.ZEPHYR_PLATFORM_DIR}/build/"
            "twister-tests-platform_api-native_sim-coverage",
        )

    def test_coverage_uses_a_separate_output_tree(self):
        coverage = MODULE.twister_command(
            Path("tests/platform_api"), "native_sim", False, coverage=True
        )
        plain = MODULE.twister_command(
            Path("tests/platform_api"), "native_sim", False, coverage=False
        )

        coverage_outdir = (
            f"--outdir {MODULE.HERE}/build/"
            "twister-tests-platform_api-native_sim-coverage"
        )
        plain_outdir = (
            f"--outdir {MODULE.HERE}/build/"
            "twister-tests-platform_api-native_sim"
        )
        self.assertIn(coverage_outdir, coverage)
        self.assertIn(plain_outdir, plain)
        self.assertNotIn(f"{plain_outdir}-coverage", plain)
        self.assertNotIn("--coverage", plain.split())

    def test_plain_and_coverage_outputs_include_selected_scenario(self):
        scenario = "wamr.zephyr.platform_api.kernel"
        slug = "wamr-zephyr-platform-api-kernel"
        coverage = MODULE.twister_command(
            Path("tests/platform_api"),
            "native_sim",
            False,
            coverage=True,
            scenario=scenario,
        )
        plain = MODULE.twister_command(
            Path("tests/platform_api"),
            "native_sim",
            False,
            scenario=scenario,
        )

        base = f"{MODULE.HERE}/build/twister-tests-platform_api-native_sim-{slug}"
        base += "-af729dc6"
        self.assertIn(f"--outdir {base}-coverage", coverage)
        self.assertIn(f"--outdir {base}", plain)


if __name__ == "__main__":
    unittest.main()

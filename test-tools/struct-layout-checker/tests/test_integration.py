"""Integration tests for source subcommand (requires gcc + wasi-sdk)."""
import os
import shutil
import subprocess
import sys
import pytest

WASI_SDK = "/opt/wasi-sdk"
TOOL_DIR = os.path.join(os.path.dirname(__file__), "..")
SAMPLE_DIR = os.path.join(TOOL_DIR, "..", "..", "samples",
                          "wasm-host-struct-consistent")
NATIVE_IMPL = os.path.join(SAMPLE_DIR, "src", "native_impl.c")

has_gcc = shutil.which("gcc") is not None
has_wasi_sdk = os.path.isfile(os.path.join(WASI_SDK, "bin", "clang"))

pytestmark = pytest.mark.skipif(
    not (has_gcc and has_wasi_sdk),
    reason="Requires gcc and wasi-sdk at /opt/wasi-sdk"
)


def run_tool(*args):
    cmd = [sys.executable, os.path.join(TOOL_DIR, "main.py")] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=60)


class TestSourceSubcommand:
    def test_detects_mismatch(self):
        result = run_tool(
            "source",
            "--source", NATIVE_IMPL,
            "--native-cc", "gcc",
            "--native-flags=-fshort-enums",
            "--wasi-sdk", WASI_SDK,
            "--verbose",
        )
        assert result.returncode == 1
        assert "MISMATCH" in result.stdout
        assert "device_report" in result.stdout

    def test_finds_structs(self):
        result = run_tool(
            "source",
            "--source", NATIVE_IMPL,
            "--native-cc", "gcc",
            "--native-flags=-fshort-enums",
            "--wasi-sdk", WASI_SDK,
        )
        assert "sensor_report" in result.stdout
        assert "device_report" in result.stdout

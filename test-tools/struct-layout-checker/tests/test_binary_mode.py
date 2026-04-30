"""Integration tests for binary subcommand (requires gcc + wasi-sdk)."""
import os
import shutil
import subprocess
import sys
import pytest

WASI_SDK = "/opt/wasi-sdk"
TOOL_DIR = os.path.join(os.path.dirname(__file__), "..")
SAMPLE_DIR = os.path.join(TOOL_DIR, "..", "..", "samples",
                          "wasm-host-struct-consistent")
SHARED_DIR = os.path.join(SAMPLE_DIR, "shared")

has_gcc = shutil.which("gcc") is not None
has_wasi_sdk = os.path.isfile(os.path.join(WASI_SDK, "bin", "clang"))

pytestmark = pytest.mark.skipif(
    not (has_gcc and has_wasi_sdk),
    reason="Requires gcc and wasi-sdk at /opt/wasi-sdk"
)

PROBE_SRC = """\
#include <stdint.h>
#include "struct_consistent.h"
#include "struct_inconsistent.h"

struct sensor_report __attribute__((used)) __probe_sr;
struct device_report __attribute__((used)) __probe_dr;
"""


def run_tool(*args):
    cmd = [sys.executable, os.path.join(TOOL_DIR, "main.py")] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=60)


@pytest.fixture(scope="module")
def binaries(tmp_path_factory):
    """Build debug native .o and debug wasm .o for testing."""
    tmpdir = tmp_path_factory.mktemp("binary_test")
    src = tmpdir / "probe.c"
    src.write_text(PROBE_SRC)

    native_o = str(tmpdir / "probe_native.o")
    wasm_o = str(tmpdir / "probe_wasm.o")

    subprocess.run(
        ["gcc", "-fshort-enums", "-g", "-c", str(src), "-o", native_o,
         "-I", SHARED_DIR],
        check=True, timeout=30,
    )
    subprocess.run(
        [os.path.join(WASI_SDK, "bin", "clang"), "--target=wasm32",
         "-g", "-c", str(src), "-o", wasm_o, "-I", SHARED_DIR],
        check=True, timeout=30,
    )
    return {"native": native_o, "wasm": wasm_o}


class TestBinarySubcommand:
    def test_detects_mismatch(self, binaries):
        result = run_tool(
            "binary",
            "--native", binaries["native"],
            "--wasm", binaries["wasm"],
            "--verbose",
        )
        assert result.returncode == 1
        assert "MISMATCH" in result.stdout

    def test_struct_filter(self, binaries):
        result = run_tool(
            "binary",
            "--native", binaries["native"],
            "--wasm", binaries["wasm"],
            "--structs", "sensor_report",
            "--verbose",
        )
        assert "sensor_report" in result.stdout
        assert result.returncode == 0

    def test_auto_intersect(self, binaries):
        result = run_tool(
            "binary",
            "--native", binaries["native"],
            "--wasm", binaries["wasm"],
        )
        assert "shared struct" in result.stdout

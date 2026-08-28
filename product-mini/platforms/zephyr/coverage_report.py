#!/usr/bin/env python3
#
# Copyright (C) 2026 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Aggregate focused gcovr coverage traces for WAMR's Zephyr platform."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


PLATFORM_FILTER = "core/shared/platform/zephyr/"
HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = HERE.parents[2]
DEFAULT_OUTPUT_DIR = HERE / "build" / "coverage-zephyr-platform-aggregate"


def validate_trace(path: Path, root: Path) -> Path:
    """Validate one gcovr JSON trace and return its resolved path."""
    resolved = path.resolve()
    try:
        trace = json.loads(resolved.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid coverage trace: {path}") from error

    files = trace.get("files") if isinstance(trace, dict) else None
    if not isinstance(files, list) or not any(
        isinstance(entry, dict)
        and isinstance(entry.get("file"), str)
        and PLATFORM_FILTER in entry["file"]
        for entry in files
    ):
        raise ValueError(f"coverage trace has no Zephyr platform files: {path}")
    return resolved


def gcovr_base_command(root: Path, traces: list[Path]) -> list[str]:
    """Build the shared, focused arguments for gcovr report generation."""
    command = ["gcovr", "-r", str(root), "--filter", PLATFORM_FILTER]
    for trace in traces:
        command.extend(["--add-tracefile", str(trace)])
    return command


def _absolute_unresolved(path: Path) -> Path:
    return Path(os.path.abspath(path))


def _reject_symlink_components(path: Path) -> None:
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            raise ValueError(f"unsafe symlink in output path: {current}")


def _reject_input_artifact(path: Path, traces: list[Path]) -> None:
    if any(path == trace or path in trace.parents for trace in traces):
        raise ValueError(f"output path contains an input artifact: {path}")


def _validate_output_paths(
    output_dir: Path, traces: list[Path]
) -> tuple[Path, Path, Path]:
    output_dir = _absolute_unresolved(output_dir)
    if not output_dir.name:
        raise ValueError("output directory must not be the filesystem root")
    temporary = output_dir.with_name(f"{output_dir.name}.tmp")
    backup = output_dir.with_name(f"{output_dir.name}.bak")
    for path in (output_dir, temporary, backup):
        _reject_symlink_components(path)
        _reject_input_artifact(path, traces)
    return output_dir, temporary, backup


def _remove_temporary_directory(
    temporary: Path, parent: Path, traces: list[Path]
) -> None:
    if temporary.parent != parent:
        raise ValueError(f"temporary output is outside requested parent: {temporary}")
    _reject_symlink_components(temporary)
    _reject_input_artifact(temporary, traces)
    if not temporary.exists():
        return
    if not temporary.is_dir() or temporary.is_symlink():
        raise ValueError(f"temporary output is not a directory: {temporary}")
    shutil.rmtree(temporary)


def _remove_backup_directory(backup: Path, parent: Path, traces: list[Path]) -> None:
    if backup.parent != parent:
        raise ValueError(f"backup output is outside requested parent: {backup}")
    _reject_symlink_components(backup)
    _reject_input_artifact(backup, traces)
    if not backup.exists():
        return
    if not backup.is_dir() or backup.is_symlink():
        raise ValueError(f"backup output is not a directory: {backup}")
    shutil.rmtree(backup)


def aggregate_coverage(root: Path, traces: list[Path], output_dir: Path) -> bool:
    """Generate and atomically publish focused reports for validated traces."""
    resolved_traces = [validate_trace(trace, root) for trace in traces]
    if len(set(resolved_traces)) != len(resolved_traces):
        raise ValueError("duplicate resolved coverage trace path")

    output_dir, temporary, backup = _validate_output_paths(output_dir, resolved_traces)
    output_parent = output_dir.parent
    output_parent.mkdir(parents=True, exist_ok=True)
    _remove_temporary_directory(temporary, output_parent, resolved_traces)
    if backup.exists() or backup.is_symlink():
        raise ValueError(f"backup output already exists: {backup}")
    temporary.mkdir()

    base_command = gcovr_base_command(root, resolved_traces)
    reports = [
        ["--html-details", str(temporary / "index.html")],
        ["--xml-pretty", "--xml", str(temporary / "coverage.xml")],
        ["--json-pretty", "--json", str(temporary / "coverage.json")],
        ["--txt-metric", "line", "--txt", str(temporary / "line-summary.txt")],
        ["--txt-metric", "branch", "--txt", str(temporary / "branch-summary.txt")],
    ]
    try:
        for report in reports:
            subprocess.run(base_command + report, check=True, cwd=root)
        (temporary / "inputs.txt").write_text(
            "".join(f"{trace}\n" for trace in resolved_traces)
        )

        previous_output = output_dir.exists()
        if previous_output:
            if not output_dir.is_dir() or output_dir.is_symlink():
                raise ValueError(f"output is not a directory: {output_dir}")
            output_dir.replace(backup)
        temporary.replace(output_dir)
        if previous_output:
            _remove_backup_directory(backup, output_parent, resolved_traces)
    except Exception:
        if backup.exists() and not output_dir.exists():
            backup.replace(output_dir)
        _remove_temporary_directory(temporary, output_parent, resolved_traces)
        raise
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", metavar="TRACE", type=Path)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()

    try:
        aggregate_coverage(args.root, args.traces, args.output_dir)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"coverage aggregation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

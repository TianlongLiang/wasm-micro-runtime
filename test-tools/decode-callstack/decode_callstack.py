#!/usr/bin/env python3
# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
Decode WAMR call stack output using a debug companion WASM binary.

Converts runtime-reported bytecode offsets into full inlined call stacks
using llvm-addr2line against a debug companion that retains DWARF info.

This handles the case where the production WASM is fully optimized (-Oz)
and stripped (no name section, no DWARF), but a debug companion built
from the same intermediate with `wasm-opt -Oz -g` is available offline.
"""

import argparse
import os
import re
import subprocess
import sys


def parse_callstack(lines):
    """
    Parse WAMR call stack lines.

    Accepts formats:
        #00: 0x0038 - func_name
        #00: 0x0038 - $f0
        #00  $f0

    Returns list of (frame_num, file_offset, raw_name) tuples.
    file_offset is None for lines without an address.
    """
    pattern = re.compile(
        r"#(\d+)(?::\s*0x([0-9a-fA-F]+))?\s*-?\s*(.*)"
    )
    frames = []
    for line in lines:
        line = line.strip()
        m = pattern.match(line)
        if m:
            frame_num = int(m.group(1))
            offset = int(m.group(2), 16) if m.group(2) else None
            name = m.group(3).strip() if m.group(3) else ""
            frames.append((frame_num, offset, name))
    return frames


def get_code_section_offset(wasm_objdump, wasm_path):
    """
    Get the file offset where the Code section starts in a WASM binary.

    Runs: wasm-objdump -h <wasm_path>
    Parses the line: "Code start=0x000000XX ..."
    Returns the start offset as an integer.
    """
    result = subprocess.run(
        [wasm_objdump, "-h", wasm_path],
        capture_output=True, text=True, check=True
    )
    for line in result.stdout.splitlines():
        if "Code" in line and "start=" in line:
            m = re.search(r"start=(0x[0-9a-fA-F]+)", line)
            if m:
                return int(m.group(1), 16)
    raise RuntimeError(
        f"Could not find Code section in {wasm_path}. "
        f"Is it a valid WASM binary?"
    )


def resolve_inlines(llvm_addr2line, debug_wasm, dwarf_addrs):
    """
    Resolve DWARF addresses to inlined call stacks.

    Calls llvm-addr2line -e <debug_wasm> -f -i once per address.

    Returns a list of lists. Each inner list contains (func_name, location)
    tuples representing the inline chain from innermost to outermost.
    """
    if not dwarf_addrs:
        return []

    all_resolved = []
    for addr in dwarf_addrs:
        addr_str = f"0x{addr:x}"
        result = subprocess.run(
            [llvm_addr2line, "-e", debug_wasm, "-f", "-i", addr_str],
            capture_output=True, text=True, check=True
        )
        lines = result.stdout.strip().splitlines()
        chain = []
        for i in range(0, len(lines) - 1, 2):
            func_name = lines[i].strip()
            location = lines[i + 1].strip()
            chain.append((func_name, location))
        all_resolved.append(chain)

    return all_resolved


def format_output(frames, resolved):
    """
    Format resolved inline chains grouped under runtime frames.
    """
    output_lines = []
    for i, (frame_num, offset, raw_name) in enumerate(frames):
        if offset is not None:
            output_lines.append(f"#{frame_num:02d}: 0x{offset:04x} - {raw_name}")
        else:
            output_lines.append(f"#{frame_num:02d}: {raw_name}")

        if i < len(resolved) and resolved[i]:
            chain = resolved[i]
            if len(chain) > 1 or (chain[0][0] != "??" and chain[0][1] != "??:0"):
                output_lines.append("  Inlined call stack:")
                for func_name, location in chain:
                    if func_name == "??":
                        func_name = "(unknown)"
                    output_lines.append(f"    {func_name}")
                    if location and location != "??:0":
                        output_lines.append(f"        at {location}")
        output_lines.append("")

    return "\n".join(output_lines)


def main():
    parser = argparse.ArgumentParser(
        description="Decode WAMR call stack using a debug companion WASM binary. "
                    "Resolves runtime bytecode offsets to full inlined call stacks."
    )
    parser.add_argument(
        "call_stack_file",
        help="Path to file containing WAMR call stack output"
    )
    parser.add_argument(
        "--debug-wasm", required=True,
        help="Path to debug companion .wasm (built with wasm-opt -Oz -g)"
    )
    parser.add_argument(
        "--wasi-sdk", default="/opt/wasi-sdk",
        help="Path to wasi-sdk (for llvm-addr2line)"
    )
    parser.add_argument(
        "--wabt", default="/opt/wabt",
        help="Path to wabt (for wasm-objdump)"
    )
    args = parser.parse_args()

    # Resolve tool paths
    llvm_addr2line = os.path.join(args.wasi_sdk, "bin", "llvm-addr2line")
    wasm_objdump = os.path.join(args.wabt, "bin", "wasm-objdump")

    # Validate tools exist
    if not os.path.isfile(llvm_addr2line):
        print(f"Error: llvm-addr2line not found at {llvm_addr2line}", file=sys.stderr)
        print("Install wasi-sdk or set --wasi-sdk path.", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(wasm_objdump):
        print(f"Error: wasm-objdump not found at {wasm_objdump}", file=sys.stderr)
        print("Install wabt or set --wabt path.", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.debug_wasm):
        print(f"Error: Debug companion not found at {args.debug_wasm}", file=sys.stderr)
        sys.exit(1)

    # Read call stack
    with open(args.call_stack_file, "r") as f:
        lines = f.readlines()

    frames = parse_callstack(lines)
    if not frames:
        print("No WAMR call stack frames found in input.", file=sys.stderr)
        sys.exit(0)

    # Get code section offset from debug companion
    code_section_start = get_code_section_offset(wasm_objdump, args.debug_wasm)

    # Convert file offsets to DWARF addresses.
    # Subtract 1 because WAMR reports ip AFTER advancing past the faulting
    # instruction (like a return address). The DWARF ranges cover the
    # instruction itself, not the byte after it.
    dwarf_addrs = []
    for frame_num, offset, name in frames:
        if offset is not None and offset > code_section_start:
            dwarf_addrs.append(offset - code_section_start - 1)
        else:
            dwarf_addrs.append(0)

    # Resolve inlines
    resolved = resolve_inlines(llvm_addr2line, args.debug_wasm, dwarf_addrs)

    # Format and print
    print(format_output(frames, resolved))


if __name__ == "__main__":
    main()

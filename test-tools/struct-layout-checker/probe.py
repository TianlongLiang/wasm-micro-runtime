"""Probe generation and dual compilation for source-based checking.

Generates a tiny C file that forces each struct into DWARF debug info,
then compiles it with both native and wasm32 compilers.
"""
import os
import subprocess
import tempfile


# The probe is a minimal C file that declares one global variable per struct.
# __attribute__((used)) prevents the compiler from dead-code-eliminating it,
# ensuring the struct definition appears in DWARF debug info.
# We only need the struct *definition* — no function bodies, no linking.
def generate_probe_source(structs, extra_includes=None):
    """Generate a C file declaring one global per struct for DWARF."""
    lines = ["#include <stdint.h>", "#include <stddef.h>"]
    if extra_includes:
        lines.extend(f'#include "{inc}"' for inc in extra_includes)
    lines.append("")
    for s in structs:
        name = s["struct_name"]
        lines.append(f'struct {name} __attribute__((used)) __probe_{name};')
    lines.append("")
    return "\n".join(lines)


# Compile with -g (debug info) and -c (compile only, no link).
# -c is critical: the probe includes struct headers that may reference
# unknown symbols (e.g., WAMR types). Since we never link, these
# unresolved symbols don't matter — we only care about the DWARF metadata.
def compile_probe(source_text, output_path, compiler, flags=None):
    """Compile probe source to .o with debug info (-g -c).

    Returns True on success, False on failure.
    """
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c',
                                     delete=False) as f:
        f.write(source_text)
        src_path = f.name

    try:
        cmd = [compiler] + (flags or []) + ["-g", "-c", src_path,
                                             "-o", output_path]
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=30)
        if result.returncode != 0:
            print(f"  Compile failed: {' '.join(cmd)}")
            print(f"  stderr: {result.stderr.strip()}")
            return False
        return True
    finally:
        os.unlink(src_path)

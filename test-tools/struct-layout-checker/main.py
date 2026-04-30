#!/usr/bin/env python3
"""WAMR Struct Layout Checker — CLI entry point.

Subcommands:
  source  — discover structs from NativeSymbol arrays in C source
  binary  — compare struct layouts from pre-built native + WASM files
"""
import argparse
import os
import sys
import tempfile

from discovery import discover_structs
from probe import generate_probe_source, compile_probe
from dwarf import extract_struct_layouts, llvm_dwarfdump_or_default
from compare import compare_layouts


# Common libc/system structs that appear in both native and WASM DWARF
# but are not WAMR API structs. Filtered out in binary mode's auto-intersect
# to reduce noise. Can be disabled with --no-filter.
LIBC_BLOCKLIST = frozenset({
    "stat", "timespec", "timeval", "tm", "sockaddr", "sockaddr_in",
    "sockaddr_in6", "sockaddr_storage", "iovec", "addrinfo", "pollfd",
    "dirent", "sigaction", "sigset_t", "pthread_attr_t", "sched_param",
    "rusage", "rlimit", "statfs", "statvfs", "utsname", "passwd",
    "group", "hostent", "servent", "protoent", "netent", "lconv",
    "flock", "termios", "winsize",
})


# Shared by both subcommands. Starts with the discovered/requested struct
# names, compares each one, and when a member is a nested struct, adds it
# to the check queue for recursive comparison. Tracks which structs have
# been checked to avoid infinite loops with mutually-referencing structs.
def _run_check_loop(struct_names, native_layouts, wasm_layouts,
                     verbose, quiet):
    """Recursive comparison loop shared by both subcommands."""
    total_mismatches = 0
    all_suggestions = []
    checked = set()
    to_check = list(struct_names)

    while to_check:
        name = to_check.pop(0)
        if name in checked:
            continue
        checked.add(name)

        if name not in native_layouts:
            if name in struct_names:
                print(f"\n  WARNING: struct {name} not found in "
                      f"native DWARF")
                total_mismatches += 1
            continue
        if name not in wasm_layouts:
            if name in struct_names:
                print(f"\n  WARNING: struct {name} not found in "
                      f"wasm DWARF")
                total_mismatches += 1
            continue

        if not quiet and name not in struct_names:
            print(f"  Also checking nested struct: {name}")

        count, suggestions, nested = compare_layouts(
            name, native_layouts[name], wasm_layouts[name],
            verbose=verbose)
        total_mismatches += count
        all_suggestions.extend(suggestions)

        for ns in nested:
            if ns not in checked:
                to_check.append(ns)

    return total_mismatches, all_suggestions, checked


# Source-based flow (original behavior):
#   1. Parse NativeSymbol arrays in C source to find registered native functions
#   2. Decode WAMR signatures to identify which parameters are struct pointers
#   3. Generate a probe .c file and compile it with both native and wasm32 compilers
#   4. Extract DWARF struct layouts from both .o files
#   5. Compare layouts field-by-field, report mismatches with fix suggestions
def cmd_source(args):
    """Source-based checking: NativeSymbol discovery -> probe -> compare."""
    verbose = args.verbose
    quiet = args.quiet

    wasm_clang = os.path.join(args.wasi_sdk, "bin", "clang")
    if not os.path.isfile(wasm_clang):
        print(f"Error: wasi-sdk clang not found at {wasm_clang}")
        return 2

    all_structs = []
    all_unchecked_ptrs = []
    for src in args.source:
        if not quiet:
            print(f"Scanning {src} for NativeSymbol arrays...")
        structs, unchecked = discover_structs(src, args.include_dir)
        if not quiet:
            for s in structs:
                print(f"  Found: {s['from_func']}  → struct {s['struct_name']}")
        all_structs.extend(structs)
        all_unchecked_ptrs.extend(unchecked)

    if args.structs:
        discovered = {s["struct_name"]: s for s in all_structs}
        all_structs = [
            discovered.get(n.strip(), {
                "struct_name": n.strip(), "header_path": None,
                "from_func": "(manual)", "from_export": "(manual)",
            })
            for n in args.structs.split(",")
        ]

    if not all_structs:
        if not quiet:
            print("No structs to check.")
        return 0

    headers = []
    include_dirs = set()
    for s in all_structs:
        if s["header_path"] and s["header_path"] not in headers:
            headers.append(s["header_path"])
            include_dirs.add(os.path.dirname(s["header_path"]))
    include_dirs = list(include_dirs) + args.include_dir

    probe_src = generate_probe_source(all_structs, extra_includes=headers)
    if verbose:
        print(f"\n--- Probe source ---\n{probe_src}---")

    struct_names = [s["struct_name"] for s in all_structs]

    native_flags = (args.native_flags.split() if args.native_flags else [])
    wasm_flags = ["--target=wasm32"] + (
        args.wasm_flags.split() if args.wasm_flags else [])
    for d in include_dirs:
        native_flags.extend(["-I", d])
        wasm_flags.extend(["-I", d])

    with tempfile.TemporaryDirectory() as tmpdir:
        native_obj = os.path.join(tmpdir, "probe_native.o")
        wasm_obj = os.path.join(tmpdir, "probe_wasm.o")

        if not quiet:
            print(f"\nCompiling probe for native ({args.native_cc})...")
        if not compile_probe(probe_src, native_obj, args.native_cc,
                             native_flags):
            return 2

        if not quiet:
            print(f"Compiling probe for wasm32 ({wasm_clang})...")
        if not compile_probe(probe_src, wasm_obj, wasm_clang, wasm_flags):
            return 2

        llvm_dd = os.path.join(args.wasi_sdk, "bin", "llvm-dwarfdump")
        if not os.path.isfile(llvm_dd):
            llvm_dd = "llvm-dwarfdump"

        native_layouts = extract_struct_layouts(native_obj, None)
        wasm_layouts = extract_struct_layouts(wasm_obj, None, llvm_dd)

        total_mismatches, all_suggestions, checked = _run_check_loop(
            struct_names, native_layouts, wasm_layouts, verbose, quiet)

    if all_unchecked_ptrs:
        print("\nUnchecked pointer parameters (layout cannot be verified):")
        for u in all_unchecked_ptrs:
            print(f"  {u['func_name']} param {u['param_idx']}: "
                  f"{u['param_type']}")
        print("\nVOID_PTR_WARNINGS_BEGIN")
        for u in all_unchecked_ptrs:
            print(f"  - {u['func_name']}: '{u['param_type']}'"
                  f" — layout cannot be verified automatically")
        print("VOID_PTR_WARNINGS_END")

    print(f"\nSummary: {len(checked)} struct(s) checked "
          f"({len(struct_names)} top-level + "
          f"{len(checked) - len(struct_names)} nested), "
          f"{total_mismatches} mismatch(es)")

    if all_suggestions:
        print("\nFIX_SUGGESTIONS_BEGIN")
        for s in all_suggestions:
            print(f"  - {s}")
        print("FIX_SUGGESTIONS_END")

    return 0 if total_mismatches == 0 else 1


# Binary-based flow (new):
#   1. Extract all struct layouts from pre-built native file (ELF) via pyelftools
#   2. Extract all struct layouts from pre-built WASM file via llvm-dwarfdump
#   3. Intersect struct names to find shared types (or use --structs override)
#   4. Filter out common libc structs (unless --no-filter)
#   5. Compare layouts field-by-field, same output as source mode
# Both files must have been compiled with -g (debug info).
def cmd_binary(args):
    """Binary-based checking: extract DWARF from pre-built files."""
    verbose = args.verbose
    quiet = args.quiet

    llvm_dd = llvm_dwarfdump_or_default(args.llvm_dwarfdump)

    if not quiet:
        print(f"Extracting struct layouts from {args.native}...")
    native_layouts = extract_struct_layouts(args.native, None)
    if not native_layouts:
        print(f"Error: no DWARF struct info in {args.native}. "
              f"Rebuild with -g.")
        return 2

    if not quiet:
        print(f"Extracting struct layouts from {args.wasm}...")
    wasm_layouts = extract_struct_layouts(args.wasm, None, llvm_dd)
    if not wasm_layouts:
        print(f"Error: no DWARF struct info in {args.wasm}. "
              f"Rebuild with -g.")
        return 2

    if args.structs:
        struct_names = [n.strip() for n in args.structs.split(",")]
    else:
        native_names = set(native_layouts.keys())
        wasm_names = set(wasm_layouts.keys())
        common = native_names & wasm_names
        if not args.no_filter:
            common -= LIBC_BLOCKLIST
        struct_names = sorted(common)

        if not quiet:
            print(f"\nFound {len(common)} shared struct(s) "
                  f"(from {len(native_names)} native, "
                  f"{len(wasm_names)} wasm)")
            if not struct_names:
                print("  No shared structs to compare.")

    if not struct_names:
        if not quiet:
            print("No structs to check.")
        return 0

    total_mismatches, all_suggestions, checked = _run_check_loop(
        struct_names, native_layouts, wasm_layouts, verbose, quiet)

    print(f"\nSummary: {len(checked)} struct(s) checked "
          f"({len(struct_names)} top-level + "
          f"{len(checked) - len(struct_names)} nested), "
          f"{total_mismatches} mismatch(es)")

    if all_suggestions:
        print("\nFIX_SUGGESTIONS_BEGIN")
        for s in all_suggestions:
            print(f"  - {s}")
        print("FIX_SUGGESTIONS_END")

    return 0 if total_mismatches == 0 else 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="WAMR Struct Layout Checker")
    sub = parser.add_subparsers(dest="command")

    # source subcommand
    p_src = sub.add_parser("source",
                           help="Check from C source with NativeSymbol arrays")
    p_src.add_argument("--source", required=True, nargs="+",
                       help="C source file(s)")
    p_src.add_argument("--include-dir", action="append", default=[],
                       help="Additional include dirs")
    p_src.add_argument("--native-cc", required=True,
                       help="Native C compiler")
    p_src.add_argument("--native-flags", default="",
                       help="Extra native compiler flags")
    p_src.add_argument("--wasi-sdk", required=True,
                       help="Path to wasi-sdk")
    p_src.add_argument("--wasm-flags", default="",
                       help="Extra wasm compiler flags")
    p_src.add_argument("--structs",
                       help="Override: comma-separated struct names")
    p_src.add_argument("--verbose", action="store_true")
    p_src.add_argument("--quiet", action="store_true")

    # binary subcommand
    p_bin = sub.add_parser("binary",
                           help="Check from pre-built native + WASM files")
    p_bin.add_argument("--native", required=True,
                       help="Native ELF file (.o, .so, executable)")
    p_bin.add_argument("--wasm", required=True,
                       help="WASM file (.wasm)")
    p_bin.add_argument("--structs",
                       help="Override: comma-separated struct names")
    p_bin.add_argument("--no-filter", action="store_true",
                       help="Don't filter out libc struct names")
    p_bin.add_argument("--llvm-dwarfdump",
                       help="Path to llvm-dwarfdump")
    p_bin.add_argument("--verbose", action="store_true")
    p_bin.add_argument("--quiet", action="store_true")

    args = parser.parse_args(argv)

    if args.command == "source":
        return cmd_source(args)
    elif args.command == "binary":
        return cmd_binary(args)
    else:
        parser.print_help()
        return 2


if __name__ == "__main__":
    sys.exit(main())

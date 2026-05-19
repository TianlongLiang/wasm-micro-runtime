#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
extract_log_strings.py - Preprocessor-based multi-file extraction for
dictionary logging.

Workflow:
  1. Dry-run compile (clang -fsyntax-only) to verify sources are valid.
  2. Preprocess each source file (clang -E) to expand all macros.
  3. Scan preprocessed output for wasm_log(level, "fmt", args) calls.
  4. Assign sequential IDs, compute type descriptors, transform calls.
  5. Output one <name>_dict.i per input file + one merged JSON dictionary.
"""

import argparse
import json
import os
import re
import subprocess
import sys


# Type codes for arg_type_descriptor nibbles
TYPE_INT32 = 0x01
TYPE_INT64 = 0x02
TYPE_FLOAT64 = 0x03
TYPE_STRING = 0x04

# Regex to match the start of a wasm_log call (after preprocessing)
WASM_LOG_PATTERN = re.compile(r'\bwasm_log\s*\(')

# Regex for format specifiers
FORMAT_SPEC_PATTERN = re.compile(
    r'%'
    r'(?:%'                          # %% literal percent
    r'|[-+ #0]*'                     # flags
    r'(?:\*|\d+)?'                   # width
    r'(?:\.(?:\*|\d+))?'            # precision
    r'(?:ll|l|h|hh|z|j|t)?'        # length modifier
    r'[diouxXeEfFgGaAcspn])'       # conversion
)

# Regex to parse #line directives from preprocessor output
# Matches: # <line_number> "<filename>"
LINE_DIRECTIVE_PATTERN = re.compile(r'^#\s+(\d+)\s+"([^"]*)"', re.MULTILINE)


def classify_specifier(spec):
    """
    Classify a printf format specifier into a type code.
    Returns None for %% (literal percent, not an argument).
    """
    if spec == '%%':
        return None

    conv = spec[-1]

    if conv == 's':
        return TYPE_STRING

    if conv in ('f', 'e', 'g', 'F', 'E', 'G', 'a', 'A'):
        return TYPE_FLOAT64

    if conv in ('d', 'i', 'u', 'x', 'X', 'o', 'c', 'p', 'n'):
        remainder = spec[1:-1]  # strip % and conversion
        if 'll' in remainder:
            return TYPE_INT64
        if remainder.endswith('l'):
            return TYPE_INT64
        return TYPE_INT32

    return TYPE_INT32


def type_code_to_name(code):
    """Convert a type code to a human-readable name for JSON output."""
    return {
        TYPE_INT32: "int32",
        TYPE_INT64: "int64",
        TYPE_FLOAT64: "float64",
        TYPE_STRING: "string",
    }.get(code, "unknown")


def build_type_descriptor(type_codes):
    """
    Build a uint32 type descriptor from a list of type codes.
    4 bits per arg, packed right-to-left (first arg in lowest nibble).
    Max 8 args.
    """
    descriptor = 0
    for i, code in enumerate(type_codes[:8]):
        descriptor |= (code & 0x0F) << (i * 4)
    return descriptor


def find_matching_paren(source, start_pos):
    """
    Starting from source[start_pos] which should be '(', find the matching ')'.
    Handles nested parens, string literals, and character literals.
    Returns the index of the matching ')'.
    """
    depth = 0
    i = start_pos
    in_string = False
    in_char = False
    length = len(source)

    while i < length:
        c = source[i]

        if in_string:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if c == '\\':
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue

        if c == '"':
            in_string = True
        elif c == "'":
            in_char = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i

        i += 1

    return -1


def resolve_source_location(source, pos):
    """
    Given a position in the preprocessed source, find the original source file
    and line number by searching backwards for the nearest # line directive.
    Returns (filename, line_number).
    """
    # Find all line directives before this position
    text_before = source[:pos]
    last_directive = None
    for m in LINE_DIRECTIVE_PATTERN.finditer(text_before):
        last_directive = m

    if last_directive is None:
        # Fallback: count lines from the start
        line_in_pp = text_before.count('\n') + 1
        return ("<unknown>", line_in_pp)

    directive_line_num = int(last_directive.group(1))
    directive_file = last_directive.group(2)

    # Count newlines between the directive and our position
    text_after_directive = source[last_directive.end():pos]
    lines_after = text_after_directive.count('\n')

    return (directive_file, directive_line_num + lines_after)


def parse_inner_content(inner):
    """
    Parse the inner content of a wasm_log(...) call.
    Expected form: <level_expr>, "<format_string>", <args...>

    Returns (level_text, format_string, args_text) or None if not parseable.
    The level_text may contain parentheses from macro expansion.
    """
    # Find the first comma at depth 0 (separating level from format string)
    depth = 0
    in_string = False
    in_char = False
    first_comma = -1
    i = 0

    while i < len(inner):
        c = inner[i]

        if in_string:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if c == '\\':
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue

        if c == '"':
            in_string = True
        elif c == "'":
            in_char = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ',' and depth == 0:
            first_comma = i
            break

        i += 1

    if first_comma < 0:
        return None

    level_text = inner[:first_comma].strip()
    rest = inner[first_comma + 1:].strip()

    # The format string should be the next thing - a C string literal
    if not rest or rest[0] != '"':
        return None  # second arg is not a string literal

    # Parse the string literal (handle escape sequences)
    i = 1  # skip opening quote
    fmt_chars = []
    while i < len(rest) and rest[i] != '"':
        if rest[i] == '\\':
            fmt_chars.append(rest[i])
            i += 1
            if i < len(rest):
                fmt_chars.append(rest[i])
                i += 1
        else:
            fmt_chars.append(rest[i])
            i += 1

    if i >= len(rest):
        return None  # unterminated string

    format_string = ''.join(fmt_chars)
    i += 1  # skip closing quote

    # Check for string concatenation (adjacent string literals)
    # After preprocessing, strings should be fully resolved, but handle it anyway
    while i < len(rest):
        while i < len(rest) and rest[i] in ' \t\n\r':
            i += 1
        if i < len(rest) and rest[i] == '"':
            # Another string literal - concatenate
            i += 1
            while i < len(rest) and rest[i] != '"':
                if rest[i] == '\\':
                    fmt_chars.append(rest[i])
                    i += 1
                    if i < len(rest):
                        fmt_chars.append(rest[i])
                        i += 1
                else:
                    fmt_chars.append(rest[i])
                    i += 1
            if i < len(rest):
                i += 1
            format_string = ''.join(fmt_chars)
        else:
            break

    # Remaining text after format string
    after_fmt = rest[i:].strip()
    if after_fmt.startswith(','):
        args_text = after_fmt[1:].strip()
    else:
        args_text = ''

    return (level_text, format_string, args_text)


def extract_wasm_log_calls(source):
    """
    Extract all wasm_log() calls from preprocessed source.
    Returns a list of dicts with call info.
    """
    results = []

    for match in WASM_LOG_PATTERN.finditer(source):
        # Skip struct/object member access: obj.wasm_log() or ptr->wasm_log()
        # These are method calls on a struct field, not our wasm_log import.
        start = match.start()
        if start > 0 and source[start - 1] == '.':
            continue
        if start > 1 and source[start - 2:start] == '->':
            continue

        paren_pos = match.end() - 1  # position of '('
        close_paren = find_matching_paren(source, paren_pos)
        if close_paren < 0:
            print(f"Warning: unmatched paren for wasm_log at offset "
                  f"{match.start()}", file=sys.stderr)
            continue

        inner = source[paren_pos + 1:close_paren]

        # Parse the call content
        parsed = parse_inner_content(inner)
        if parsed is None:
            # Second arg is not a string literal - skip silently
            continue

        level_text, format_string, args_text = parsed

        # Resolve source location from # line directives
        src_file, src_line = resolve_source_location(source, match.start())

        # Parse format specifiers
        specifiers = FORMAT_SPEC_PATTERN.findall(format_string)
        type_codes = []
        for spec in specifiers:
            tc = classify_specifier(spec)
            if tc is not None:
                type_codes.append(tc)

        # Skip if more than 8 args
        if len(type_codes) > 8:
            print(f"Error: wasm_log at {src_file}:{src_line} has "
                  f"{len(type_codes)} format args (max 8). "
                  f"Reduce arguments or split into multiple log calls.",
                  file=sys.stderr)
            sys.exit(1)

        type_names = [type_code_to_name(tc) for tc in type_codes]
        descriptor = build_type_descriptor(type_codes)

        results.append({
            'level_text': level_text,
            'fmt': format_string,
            'args_text': args_text,
            'arg_types': type_codes,
            'arg_type_names': type_names,
            'type_descriptor': descriptor,
            'source_file': src_file,
            'source_line': src_line,
            'start_pos': match.start(),
            'end_pos': close_paren + 1,
        })

    return results


def generate_replacement(call, string_id):
    """
    Generate the wasm_log_dict() replacement call.
    """
    level_text = call['level_text']
    descriptor = call['type_descriptor']
    args_text = call['args_text'].strip()

    if args_text:
        return (f"wasm_log_dict({level_text}, /*id=*/{string_id}, "
                f"/*types=*/0x{descriptor:x}, {args_text})")
    else:
        return (f"wasm_log_dict({level_text}, /*id=*/{string_id}, "
                f"/*types=*/0x{descriptor:x})")


def transform_source(source, calls, id_offset):
    """
    Replace wasm_log() calls with wasm_log_dict() calls in preprocessed source.
    Also replaces the wasm_log declaration with wasm_log_dict declaration.
    Returns the transformed source string.
    """
    # Build replacements in reverse order so positions remain valid
    replacements = []
    for i, call in enumerate(calls):
        string_id = id_offset + i
        replacement = generate_replacement(call, string_id)
        replacements.append((call['start_pos'], call['end_pos'], replacement))

    # Also replace the wasm_log function declaration with wasm_log_dict
    # The declaration in preprocessed output looks like:
    #   __attribute__((__import_module__("env")))
    #   __attribute__((__import_name__("wasm_log")))
    #   int32_t wasm_log(uint32_t log_level, const char *format, ...);
    # Replace entire block with wasm_log_dict declaration
    decl_pattern = re.compile(
        r'(__attribute__\(\(__import_module__\("env"\)\)\)\s*'
        r'__attribute__\(\(__import_name__\("wasm_log"\)\)\)\s*'
        r'int32_t\s+wasm_log\s*\([^)]*\)\s*;)',
        re.DOTALL
    )
    decl_match = decl_pattern.search(source)
    if decl_match:
        new_decl = (
            '__attribute__((__import_module__("env")))\n'
            '__attribute__((__import_name__("wasm_log_dict")))\n'
            'int32_t wasm_log_dict(unsigned int log_level, '
            'unsigned int string_id, unsigned int arg_type_descriptor, ...);'
        )
        replacements.append((decl_match.start(), decl_match.end(), new_decl))

    # Sort by position descending for reverse application
    replacements.sort(key=lambda x: x[0], reverse=True)

    result = list(source)
    for start, end, repl in replacements:
        result[start:end] = list(repl)

    transformed = ''.join(result)

    # Add WASM_LOG_DICT define at the top
    transformed = "#define WASM_LOG_DICT 1\n" + transformed

    return transformed


def run_clang_command(clang, args, description):
    """Run a clang command and return (returncode, stdout, stderr)."""
    cmd = [clang] + args
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except FileNotFoundError:
        print(f"Error: clang not found at '{clang}'", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error running {description}: {e}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Preprocessor-based multi-file log string extraction "
                    "for dictionary logging."
    )
    parser.add_argument("sources", nargs='+', metavar="FILE",
                        help="Input C source file(s)")
    parser.add_argument("--clang", default="clang",
                        help="Path to clang (default: clang)")
    parser.add_argument("--target", default=None,
                        help="Target triple (e.g. wasm32)")
    parser.add_argument("-I", dest="includes", action="append", default=[],
                        metavar="DIR", help="Include directory (repeatable)")
    parser.add_argument("-D", dest="defines", action="append", default=[],
                        metavar="MACRO", help="Preprocessor define (repeatable)")
    parser.add_argument("-o-dir", dest="output_dir", required=True,
                        help="Output directory for transformed .i files")
    parser.add_argument("-j", "--json", required=True,
                        help="Output JSON dictionary file")
    args = parser.parse_args()

    # Reject C++ source files — this tool is designed for C only.
    # C++ introduces templates, namespaces, method calls (obj.wasm_log()),
    # and overloads that the regex-based extraction cannot handle correctly.
    CPP_EXTENSIONS = ('.cpp', '.cc', '.cxx', '.C', '.c++')
    for src in args.sources:
        if src.endswith(CPP_EXTENSIONS):
            print(f"Error: C++ source files are not supported: {src}",
                  file=sys.stderr)
            print("  This tool only handles C (.c) source files.",
                  file=sys.stderr)
            print("  C++ introduces templates, namespaces, and method calls",
                  file=sys.stderr)
            print("  that cannot be handled by regex-based extraction.",
                  file=sys.stderr)
            sys.exit(1)

    # Build common clang flags
    common_flags = []
    if args.target:
        common_flags += ["--target=" + args.target]
    for inc in args.includes:
        common_flags += ["-I", inc]
    for define in args.defines:
        common_flags += ["-D", define]

    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)

    # Step 1: Dry-run compile (syntax check all sources at once)
    print("Step 1: Syntax check...", file=sys.stderr)
    syntax_args = common_flags + ["-fsyntax-only"] + args.sources
    rc, stdout, stderr = run_clang_command(args.clang, syntax_args,
                                           "syntax check")
    if rc != 0:
        print("Compilation failed:", file=sys.stderr)
        if stdout:
            sys.stdout.write(stdout)
        if stderr:
            sys.stderr.write(stderr)
        sys.exit(rc)

    # Step 2: Preprocess each source file
    print("Step 2: Preprocessing...", file=sys.stderr)
    preprocessed_files = {}
    for src in args.sources:
        basename = os.path.splitext(os.path.basename(src))[0]
        pp_path = os.path.join(args.output_dir, basename + ".i")

        preprocess_args = common_flags + ["-E", src, "-o", pp_path]
        rc, stdout, stderr = run_clang_command(args.clang, preprocess_args,
                                               f"preprocessing {src}")
        if rc != 0:
            print(f"Preprocessing failed for {src}:", file=sys.stderr)
            if stdout:
                sys.stdout.write(stdout)
            if stderr:
                sys.stderr.write(stderr)
            sys.exit(rc)

        preprocessed_files[src] = pp_path

    # Step 3: Scan and transform
    print("Step 3: Extracting log strings...", file=sys.stderr)
    all_calls = []  # (source_file, call_dict) tuples
    file_calls = {}  # src -> list of calls
    global_id = 0

    for src in args.sources:
        pp_path = preprocessed_files[src]
        with open(pp_path, 'r') as f:
            pp_source = f.read()

        calls = extract_wasm_log_calls(pp_source)

        if not calls:
            print(f"Warning: no wasm_log() calls found in {src}",
                  file=sys.stderr)

        file_calls[src] = (pp_path, pp_source, calls)
        for call in calls:
            all_calls.append((src, call))

    if not all_calls:
        print("Error: no wasm_log() calls found in any input file.",
              file=sys.stderr)
        sys.exit(1)

    # Step 4: Assign IDs and transform
    print("Step 4: Transforming...", file=sys.stderr)
    dictionary = {}
    id_offset = 0

    for src in args.sources:
        pp_path, pp_source, calls = file_calls[src]
        basename = os.path.splitext(os.path.basename(src))[0]
        out_path = os.path.join(args.output_dir, basename + "_dict.i")

        # Transform source
        transformed = transform_source(pp_source, calls, id_offset)

        # Write transformed file
        with open(out_path, 'w') as f:
            f.write(transformed)

        # Add entries to dictionary
        for i, call in enumerate(calls):
            string_id = id_offset + i
            dictionary[str(string_id)] = {
                "fmt": call['fmt'],
                "arg_types": call['arg_type_names'],
                "type_descriptor": f"0x{call['type_descriptor']:08x}",
                "source_file": call['source_file'],
                "source_line": call['source_line'],
            }

        id_offset += len(calls)

    # Step 5: Write JSON dictionary
    with open(args.json, 'w') as f:
        json.dump(dictionary, f, indent=2)
        f.write('\n')

    # Summary
    total_bytes = sum(len(call['fmt']) for _, call in all_calls)
    print(f"\nExtracted {len(all_calls)} log strings", file=sys.stderr)
    for src in args.sources:
        _, _, calls = file_calls[src]
        basename = os.path.splitext(os.path.basename(src))[0]
        print(f"  {src}: {len(calls)} calls -> "
              f"{basename}_dict.i", file=sys.stderr)
    print(f"  Dictionary: {args.json}", file=sys.stderr)
    print(f"  Total format string bytes eliminated: {total_bytes}",
          file=sys.stderr)


if __name__ == "__main__":
    main()

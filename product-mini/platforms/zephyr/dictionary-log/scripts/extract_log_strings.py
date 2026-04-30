#!/usr/bin/env python3
# Copyright (C) 2025 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""
extract_log_strings.py - Build-time string extraction for dictionary logging.

Scans WASM app C source code for LOG_* macro invocations, extracts format
strings, assigns integer IDs, infers argument types from format specifiers,
and generates:
  1. A transformed C source where LOG_* calls are replaced with wasm_log_dict()
  2. A JSON dictionary mapping string_id -> {fmt, arg_types, source_line}
"""

import argparse
import json
import re
import sys


# Log level mapping: macro suffix -> C constant name
LEVEL_MAP = {
    "ERR": "WASM_LOG_LEVEL_ERR",
    "WRN": "WASM_LOG_LEVEL_WRN",
    "INF": "WASM_LOG_LEVEL_INF",
    "DBG": "WASM_LOG_LEVEL_DBG",
    "VERBOSE": "WASM_LOG_LEVEL_VERBOSE",
}

# Type codes for arg_type_descriptor nibbles
TYPE_INT32 = 0x01
TYPE_INT64 = 0x02
TYPE_FLOAT64 = 0x03
TYPE_STRING = 0x04

# Regex to match the start of a LOG_* call
LOG_PATTERN = re.compile(r'\bLOG_(ERR|WRN|INF|DBG|VERBOSE)\s*\(')

# Regex for format specifiers (handles %%, %d, %ld, %lld, %u, %lu, %llu,
# %x, %X, %lx, %llx, %o, %lo, %llo, %f, %e, %g, %F, %E, %G, %s, %c, %p,
# %i, %li, %lli, and width/precision modifiers)
FORMAT_SPEC_PATTERN = re.compile(
    r'%'
    r'(?:%'                          # %% literal percent
    r'|[-+ #0]*'                     # flags
    r'(?:\*|\d+)?'                   # width
    r'(?:\.(?:\*|\d+))?'            # precision
    r'(?:ll|l|h|hh|z|j|t)?'        # length modifier
    r'[diouxXeEfFgGaAcspn])'       # conversion
)


def classify_specifier(spec):
    """
    Classify a printf format specifier into a type code.
    Returns None for %% (literal percent, not an argument).
    """
    if spec == '%%':
        return None

    # Check for length modifiers and conversion character
    # Strip flags, width, precision to get to length+conversion
    # We need to parse from the end
    conv = spec[-1]

    # Check if it's a string
    if conv == 's':
        return TYPE_STRING

    # Check if it's a float type
    if conv in ('f', 'e', 'g', 'F', 'E', 'G', 'a', 'A'):
        return TYPE_FLOAT64

    # For integer types, check length modifier
    if conv in ('d', 'i', 'u', 'x', 'X', 'o', 'c', 'p', 'n'):
        # Check for 'll' or 'l' before conversion
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
                i += 2  # skip escaped char
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

    return -1  # no matching paren found


def extract_format_string(args_text):
    """
    Extract the format string from the arguments of a LOG_* call.
    The format string is the first argument -- a C string literal,
    possibly composed of multiple concatenated string literals.

    Returns (format_string, rest_of_args) where rest_of_args is the
    text after the format string (starting with ',' if there are more args,
    or empty string if no more args).
    """
    text = args_text.strip()
    fmt_parts = []
    i = 0

    while i < len(text):
        # Skip whitespace
        while i < len(text) and text[i] in ' \t\n\r':
            i += 1

        if i >= len(text):
            break

        if text[i] == '"':
            # Parse a string literal
            i += 1  # skip opening quote
            literal = []
            while i < len(text) and text[i] != '"':
                if text[i] == '\\':
                    literal.append(text[i])
                    i += 1
                    if i < len(text):
                        literal.append(text[i])
                        i += 1
                else:
                    literal.append(text[i])
                    i += 1
            if i < len(text):
                i += 1  # skip closing quote
            fmt_parts.append(''.join(literal))
        else:
            # Not a string literal -- we've reached args or end
            break

    format_string = ''.join(fmt_parts)
    rest = text[i:].strip()
    if rest.startswith(','):
        rest = rest[1:].strip()
    else:
        rest = ''

    return format_string, rest


def extract_log_calls(source):
    """
    Extract all LOG_* macro calls from the source.

    Returns a list of dicts:
      {
        'level': 'ERR'|'WRN'|'INF'|'DBG'|'VERBOSE',
        'fmt': format string (without quotes),
        'args_text': raw text of the remaining arguments,
        'arg_types': list of type codes,
        'arg_type_names': list of type names for JSON,
        'type_descriptor': uint32 packed descriptor,
        'source_line': 1-based line number,
        'start_pos': position in source of LOG_ start,
        'end_pos': position in source after closing );
      }
    """
    results = []

    for match in LOG_PATTERN.finditer(source):
        level = match.group(1)
        paren_pos = match.end() - 1  # position of '('
        close_paren = find_matching_paren(source, paren_pos)
        if close_paren < 0:
            print(f"Warning: unmatched paren for LOG_{level} at offset "
                  f"{match.start()}", file=sys.stderr)
            continue

        # The full call spans from match.start() to close_paren + 1
        # But we also need to capture the trailing semicolon (if any)
        inner = source[paren_pos + 1:close_paren]

        # Calculate source line number (1-based)
        source_line = source[:match.start()].count('\n') + 1

        # Extract format string and remaining args
        fmt, args_text = extract_format_string(inner)

        # Parse format specifiers
        specifiers = FORMAT_SPEC_PATTERN.findall(fmt)
        type_codes = []
        for spec in specifiers:
            tc = classify_specifier(spec)
            if tc is not None:
                type_codes.append(tc)

        type_names = [type_code_to_name(tc) for tc in type_codes]
        descriptor = build_type_descriptor(type_codes)

        # Find the end position: include closing paren and optional semicolon
        end_pos = close_paren + 1

        results.append({
            'level': level,
            'fmt': fmt,
            'args_text': args_text,
            'arg_types': type_codes,
            'arg_type_names': type_names,
            'type_descriptor': descriptor,
            'source_line': source_line,
            'start_pos': match.start(),
            'end_pos': end_pos,
        })

    return results


def generate_replacement(call, string_id):
    """
    Generate the wasm_log_dict() replacement call for a LOG_* call.
    """
    level_const = LEVEL_MAP[call['level']]
    descriptor = call['type_descriptor']
    args_text = call['args_text'].strip()

    if args_text:
        return (f"wasm_log_dict({level_const}, /*id=*/{string_id}, "
                f"/*types=*/0x{descriptor:x}, {args_text})")
    else:
        return (f"wasm_log_dict({level_const}, /*id=*/{string_id}, "
                f"/*types=*/0x{descriptor:x})")


def transform_source(source, calls):
    """
    Replace LOG_* calls with wasm_log_dict() calls in the source.
    Adds #define WASM_LOG_DICT 1 at the very top.
    Returns the transformed source string.
    """
    # Build replacements in reverse order so positions remain valid
    replacements = []
    for i, call in enumerate(calls):
        replacement = generate_replacement(call, i)
        replacements.append((call['start_pos'], call['end_pos'], replacement))

    # Apply in reverse order
    result = list(source)
    for start, end, repl in reversed(replacements):
        result[start:end] = list(repl)

    transformed = ''.join(result)

    # Add #define WASM_LOG_DICT 1 at the very top
    transformed = "#define WASM_LOG_DICT 1\n" + transformed

    return transformed


def build_dictionary(calls):
    """
    Build the JSON dictionary mapping string_id -> {fmt, arg_types, source_line}.
    """
    dictionary = {}
    for i, call in enumerate(calls):
        dictionary[str(i)] = {
            "fmt": call['fmt'],
            "arg_types": call['arg_type_names'],
            "source_line": call['source_line'],
        }
    return dictionary


def main():
    parser = argparse.ArgumentParser(
        description="Extract LOG_* strings from WASM app C source and generate "
                    "dictionary-based logging output."
    )
    parser.add_argument("input", help="Input C source file")
    parser.add_argument("-o", "--output", required=True,
                        help="Output transformed C source file")
    parser.add_argument("-j", "--json", required=True,
                        help="Output JSON dictionary file")
    args = parser.parse_args()

    # Read input
    with open(args.input, 'r') as f:
        source = f.read()

    # Extract log calls
    calls = extract_log_calls(source)

    if not calls:
        print("No LOG_* calls found in input.", file=sys.stderr)
        sys.exit(1)

    # Generate transformed source
    transformed = transform_source(source, calls)

    # Write transformed source
    with open(args.output, 'w') as f:
        f.write(transformed)

    # Build and write JSON dictionary
    dictionary = build_dictionary(calls)
    with open(args.json, 'w') as f:
        json.dump(dictionary, f, indent=2)
        f.write('\n')

    # Calculate total format string bytes eliminated
    total_bytes = sum(len(call['fmt']) for call in calls)

    # Print summary
    print(f"Extracted {len(calls)} log strings")
    print(f"  Source: {args.output}")
    print(f"  Dictionary: {args.json}")
    print(f"  Total format string bytes eliminated: {total_bytes}")


if __name__ == "__main__":
    main()

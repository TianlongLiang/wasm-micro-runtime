"""Phase 1: Discover which structs cross the WASM-host boundary.

Parses NativeSymbol arrays in C source to find native function exports,
decodes WAMR signatures to identify pointer parameters, extracts struct
type names from C function definitions, and locates defining headers.
"""
import os
import re


# WAMR registers native APIs via NativeSymbol arrays in C source:
#   { "export_name", c_function_ptr, "signature_string", NULL }
# We regex-match this pattern to find all registered functions.
def parse_native_symbols(source_text):
    """Find all NativeSymbol entries in C source text.

    Returns list of {"export_name", "func_name", "signature"} dicts.
    """
    pattern = re.compile(
        r'\{\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*\w+\s*\}'
    )
    return [
        {
            "export_name": m.group(1),
            "func_name": m.group(2),
            "signature": m.group(3),
        }
        for m in pattern.finditer(source_text)
    ]


# WAMR signature format: "(<params>)<return>"
#   * = pointer (struct passed from WASM)
#   ~ = byte length of preceding pointer buffer
#   i/I = int32/int64, f/F = float32/float64
# We only care about * positions — those are potential struct pointers.
def signature_pointer_indices(signature):
    """Which parameters in a WAMR signature are pointers?

    Returns 0-based indices into the parameter list (after exec_env).
    Example: "(*~)i" -> [0]
    """
    m = re.match(r'\(([^)]*)\)', signature)
    if not m:
        return []
    return [i for i, ch in enumerate(m.group(1)) if ch == '*']


def find_struct_types(source_text, func_name, ptr_indices):
    """Extract struct type names from a function's pointer parameters.

    Returns (struct_types, unchecked_pointers).
    """
    # Strip C comments first — we don't want to match function signatures
    # that appear inside /* ... */ or // comments.
    cleaned = re.sub(r'/\*.*?\*/', ' ', source_text, flags=re.DOTALL)
    cleaned = re.sub(r'//[^\n]*', '', cleaned)

    pattern = re.compile(
        r'\b' + re.escape(func_name) + r'\s*\(([^)]+)\)', re.DOTALL
    )
    m = pattern.search(cleaned)
    if not m:
        return [], []

    # The first C parameter is always wasm_exec_env_t (WAMR convention),
    # which isn't part of the WAMR signature string. Skip it.
    params = [p.strip() for p in m.group(1).split(',')][1:]

    # For each pointer parameter, check if it's "struct <name> *".
    # If yes, we can verify its layout. If it's void*/char*/uint8_t*,
    # the actual struct type is only known at runtime inside the function
    # body — we flag these as "unchecked" so the user knows to verify manually.
    struct_types = []
    unchecked = []
    for idx in ptr_indices:
        if idx < len(params):
            sm = re.search(r'struct\s+(\w+)\s*\*', params[idx])
            if sm:
                struct_types.append(sm.group(1))
            else:
                unchecked.append({
                    "func_name": func_name,
                    "param_idx": idx,
                    "param_type": params[idx].strip(),
                })
    return struct_types, unchecked


# Resolve #include "..." to absolute paths. We search:
#   1. The source file's own directory
#   2. User-provided include dirs
#   3. Sibling directories (e.g., src/ -> also look in shared/)
# This handles the common layout where headers are in a sibling dir.
def find_includes(source_text, source_dir, include_dirs):
    """Resolve #include "..." directives to absolute file paths."""
    parent = os.path.dirname(source_dir)
    sibling_dirs = []
    if parent and os.path.isdir(parent):
        sibling_dirs = [
            os.path.join(parent, e) for e in os.listdir(parent)
            if os.path.isdir(os.path.join(parent, e)) and
            os.path.join(parent, e) != source_dir
        ]

    search_dirs = [source_dir] + include_dirs + sibling_dirs

    includes = []
    for m in re.finditer(r'#include\s+"([^"]+)"', source_text):
        for d in search_dirs:
            full = os.path.join(d, m.group(1))
            if os.path.isfile(full):
                includes.append(os.path.abspath(full))
                break
    return includes


def find_struct_header(struct_name, headers):
    """Find which header defines 'struct <name> {'."""
    pattern = re.compile(r'struct\s+' + re.escape(struct_name) + r'\s*\{')
    for hdr in headers:
        with open(hdr, 'r') as f:
            if pattern.search(f.read()):
                return hdr
    return None


# Full pipeline: read source -> find NativeSymbol arrays -> decode signatures
# -> match pointer params to struct types -> locate defining headers.
# Returns the structs we need to check and any unchecked void* pointers.
def discover_structs(source_path, include_dirs):
    """Full discovery: NativeSymbol arrays -> struct types -> header files.

    Returns (struct_list, unchecked_list).
    """
    with open(source_path, 'r') as f:
        source_text = f.read()

    source_dir = os.path.dirname(os.path.abspath(source_path))
    symbols = parse_native_symbols(source_text)
    if not symbols:
        print(f"  No NativeSymbol arrays found in {source_path}")
        return [], []

    # Follow includes one level deep to catch transitive headers.
    # E.g., native_impl.c includes struct_consistent.h which might
    # include another header with nested struct definitions.
    headers = find_includes(source_text, source_dir, include_dirs)
    for hdr in list(headers):
        with open(hdr, 'r') as f:
            headers.extend(find_includes(f.read(), os.path.dirname(hdr),
                                         include_dirs))
    headers = list(dict.fromkeys(headers))

    results = []
    all_unchecked = []
    for sym in symbols:
        ptr_indices = signature_pointer_indices(sym["signature"])
        if not ptr_indices:
            continue
        struct_types, unchecked = find_struct_types(
            source_text, sym["func_name"], ptr_indices)
        all_unchecked.extend(unchecked)
        for st in struct_types:
            results.append({
                "struct_name": st,
                "header_path": find_struct_header(st, headers),
                "from_func": sym["func_name"],
                "from_export": sym["export_name"],
            })

    seen = set()
    unique = [r for r in results if not (r["struct_name"] in seen or
                                         seen.add(r["struct_name"]))]
    return unique, all_unchecked

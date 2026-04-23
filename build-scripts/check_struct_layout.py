#!/usr/bin/env python3
"""
WASM-Host Struct Layout Checker

Detects struct layout mismatches between a native C compiler and wasm32-clang.

When a WASM app passes a struct pointer to a native API, the host reads
fields at offsets from the native compiler. If those offsets differ from
wasm32's layout, the host reads garbage. This tool catches such mismatches
at build time.

Pipeline:
  1. Parse NativeSymbol arrays in C source to find native function exports
  2. Extract struct types from pointer parameters in those functions
  3. Compile a probe .o with both native gcc and wasi-sdk clang (with -g)
  4. Compare DWARF debug info: field offsets, sizes, total struct size

Exit codes:
  0 = all structs match
  1 = at least one mismatch found
  2 = tool error (parse failure, compile failure, etc.)
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile


# ═══════════════════════════════════════════════════════════════════════
# Phase 1: Discover which structs cross the WASM-host boundary
# ═══════════════════════════════════════════════════════════════════════
#
# WAMR native APIs are registered via NativeSymbol arrays:
#
#   static NativeSymbol native_symbols[] = {
#       { "func_name", func_ptr, "(*~)i", NULL },
#   };
#
# The signature string tells us which parameters are pointers (*).
# We find the corresponding C function definition, look at the pointer
# parameters, and extract the struct type names.


def parse_native_symbols(source_text):
    """Find all NativeSymbol entries in C source text.

    Each entry has the form:
      { "export_name", function_pointer, "signature", attachment }

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


def signature_pointer_indices(signature):
    """Which parameters in a WAMR signature are pointers?

    WAMR signature format: "(<params>)<return>"
      * = pointer, ~ = size (follows pointer), i/I/f/F = scalar

    Returns 0-based indices into the parameter list. These map to
    C function parameters *after* wasm_exec_env_t (always param 0).

    Example: "(*~)i" → [0]  (first param after exec_env is a pointer)
    """
    m = re.match(r'\(([^)]*)\)', signature)
    if not m:
        return []
    return [i for i, ch in enumerate(m.group(1)) if ch == '*']


def find_struct_types(source_text, func_name, ptr_indices):
    """Extract struct type names from a function's pointer parameters.

    Given a function name and which parameters are pointers (from the
    WAMR signature), finds the function definition in the source and
    reads the struct type from each pointer parameter declaration.

    The first C parameter (wasm_exec_env_t) is always skipped — it's
    not part of the WAMR signature.

    Returns (struct_types, unchecked_pointers).
    unchecked_pointers: list of {"func_name", "param_idx", "param_type"}
    for non-struct pointer params (void*, char*, uint8_t*, etc.) that
    the tool cannot verify.
    """
    # Strip comments to avoid matching inside them
    cleaned = re.sub(r'/\*.*?\*/', ' ', source_text, flags=re.DOTALL)
    cleaned = re.sub(r'//[^\n]*', '', cleaned)

    # Find the function definition and extract its parameter list
    pattern = re.compile(
        r'\b' + re.escape(func_name) + r'\s*\(([^)]+)\)', re.DOTALL
    )
    m = pattern.search(cleaned)
    if not m:
        return [], []

    # Split parameters, skip first (wasm_exec_env_t)
    params = [p.strip() for p in m.group(1).split(',')][1:]

    # For each pointer index, try to match "struct <name> *" in that param.
    # If it's not a typed struct pointer (void*, char*, uint8_t*, etc.),
    # record it as an unchecked pointer — we can't verify its layout.
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


def find_includes(source_text, source_dir, include_dirs):
    """Resolve #include "..." directives to absolute file paths.

    Search order:
      1. Source file's own directory
      2. Explicitly provided include dirs
      3. Sibling directories of source dir (handles layouts like src/ + shared/)
    """
    # Auto-discover sibling directories (e.g., src/ → also search shared/)
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


def discover_structs(source_path, include_dirs):
    """Full discovery: NativeSymbol arrays → struct types → header files.

    Returns (struct_list, unchecked_list).
    struct_list: [{"struct_name", "header_path", "from_func", "from_export"}]
    unchecked_list: [{"func_name", "param_idx", "param_type"}] for void*/buffer
                    pointer params whose layout can't be verified.
    """
    with open(source_path, 'r') as f:
        source_text = f.read()

    source_dir = os.path.dirname(os.path.abspath(source_path))
    symbols = parse_native_symbols(source_text)
    if not symbols:
        print(f"  No NativeSymbol arrays found in {source_path}")
        return [], []

    # Resolve includes — follow one level deep to find transitive headers
    headers = find_includes(source_text, source_dir, include_dirs)
    for hdr in list(headers):
        with open(hdr, 'r') as f:
            headers.extend(find_includes(f.read(), os.path.dirname(hdr),
                                         include_dirs))
    headers = list(dict.fromkeys(headers))  # deduplicate, preserve order

    # For each NativeSymbol with pointer params, find the struct type
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

    # Deduplicate by struct name
    seen = set()
    unique = [r for r in results if not (r["struct_name"] in seen or
                                         seen.add(r["struct_name"]))]
    return unique, all_unchecked


# ═══════════════════════════════════════════════════════════════════════
# Phase 2: Compile probes and extract struct layouts from DWARF
# ═══════════════════════════════════════════════════════════════════════
#
# We generate a tiny C file that declares one global variable per struct.
# The __attribute__((used)) prevents the compiler from optimizing it out.
# Compiling with -g embeds DWARF debug info containing exact field offsets,
# sizes, and type names — everything we need to compare layouts.
#
# Native .o files are ELF → parsed with pyelftools.
# WASM .o files embed DWARF as custom sections → parsed with llvm-dwarfdump.


def generate_probe_source(structs, extra_includes=None):
    """Generate a C file that forces each struct into DWARF debug info."""
    lines = ["#include <stdint.h>", "#include <stddef.h>"]
    if extra_includes:
        lines.extend(f'#include "{inc}"' for inc in extra_includes)
    lines.append("")
    for s in structs:
        name = s["struct_name"]
        lines.append(f'struct {name} __attribute__((used)) __probe_{name};')
    lines.append("")
    return "\n".join(lines)


def compile_probe(source_text, output_path, compiler, flags=None):
    """Compile probe source to .o with debug info (-g -c)."""
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


def extract_struct_layouts(obj_path, struct_names=None, llvm_dwarfdump=None):
    """Extract struct layouts from DWARF in an object file.

    Auto-detects format:
      - ELF objects (native) → pyelftools
      - WASM objects → llvm-dwarfdump (DWARF embedded as custom sections)

    struct_names: list of names to extract, or None for all named structs.

    Returns: {"struct_name": {"size": N, "members": [{"name", "offset",
              "size", "type", "is_struct"}, ...]}, ...}
    """
    # Check magic bytes: b'\x00asm' = WASM, b'\\x7fELF' = ELF
    with open(obj_path, 'rb') as f:
        magic = f.read(4)

    if magic[:4] == bytes([0x00, 0x61, 0x73, 0x6d]):
        return _extract_layouts_llvm(obj_path, struct_names, llvm_dwarfdump)
    return _extract_layouts_elf(obj_path, struct_names)


def _extract_layouts_elf(obj_path, struct_names=None):
    """Extract struct layouts from ELF via pyelftools DWARF API.

    struct_names: set/list of names to extract, or None for all.
    """
    from elftools.elf.elffile import ELFFile

    layouts = {}
    wanted = set(struct_names) if struct_names else None

    with open(obj_path, 'rb') as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info():
            print(f"  No DWARF info in {obj_path}")
            return layouts

        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag != 'DW_TAG_structure_type':
                    continue
                if 'DW_AT_name' not in die.attributes:
                    continue
                name = die.attributes['DW_AT_name'].value.decode('utf-8')
                if name in layouts:
                    continue
                if wanted is not None and name not in wanted:
                    continue

                # Read struct total size
                struct_size = 0
                if 'DW_AT_byte_size' in die.attributes:
                    struct_size = die.attributes['DW_AT_byte_size'].value

                # Read each member's name, offset, size, and type name
                members = []
                for child in die.iter_children():
                    if child.tag != 'DW_TAG_member':
                        continue
                    mname = ""
                    if 'DW_AT_name' in child.attributes:
                        mname = child.attributes['DW_AT_name'] \
                            .value.decode('utf-8')

                    moffset = 0
                    if 'DW_AT_data_member_location' in child.attributes:
                        loc = child.attributes['DW_AT_data_member_location']
                        if isinstance(loc.value, int):
                            moffset = loc.value
                        elif isinstance(loc.value, list):
                            # DWARF location expression
                            moffset = loc.value[-1] if loc.value else 0

                    msize, mtype, mis_struct = _resolve_type_elf(
                        dwarf, cu, child)
                    members.append({"name": mname, "offset": moffset,
                                    "size": msize, "type": mtype,
                                    "is_struct": mis_struct})

                layouts[name] = {"size": struct_size, "members": members}

    return layouts


def _resolve_type_elf(dwarf, cu, member_die):
    """Follow DW_AT_type chain to find a member's size and type name.

    DWARF type references form a chain:
      member → typedef (uint64_t) → base_type (unsigned long long, sz=8)
    We follow until we find DW_AT_byte_size.

    Returns (byte_size, type_name, is_struct).
    is_struct is True when the resolved base type is DW_TAG_structure_type
    (i.e., this member is a nested struct).
    """
    if 'DW_AT_type' not in member_die.attributes:
        return 0, "", False

    type_offset = member_die.attributes['DW_AT_type'].value

    # Try both offset interpretations (absolute vs CU-relative)
    def get_die(offset):
        for off in [offset + cu.cu_offset, offset]:
            try:
                return cu.get_DIE_from_refaddr(off)
            except Exception:
                continue
        return None

    type_die = get_die(type_offset)
    if not type_die:
        return 0, "", False

    # Capture the first type name (e.g., "uint64_t" from the typedef)
    type_name = ""
    if 'DW_AT_name' in type_die.attributes:
        type_name = type_die.attributes['DW_AT_name'].value.decode('utf-8')

    # Follow typedef/qualifier chain to the base type with byte_size
    while type_die and type_die.tag in (
        'DW_TAG_typedef', 'DW_TAG_const_type',
        'DW_TAG_volatile_type', 'DW_TAG_restrict_type',
    ):
        if 'DW_AT_type' not in type_die.attributes:
            break
        type_die = get_die(type_die.attributes['DW_AT_type'].value)

    is_struct = (type_die is not None
                 and type_die.tag == 'DW_TAG_structure_type')

    # For struct types, use the struct's own name
    if is_struct and 'DW_AT_name' in type_die.attributes:
        type_name = type_die.attributes['DW_AT_name'].value.decode('utf-8')

    if type_die and 'DW_AT_byte_size' in type_die.attributes:
        return type_die.attributes['DW_AT_byte_size'].value, type_name, is_struct
    return 0, type_name, is_struct


def _extract_layouts_llvm(obj_path, struct_names=None, llvm_dwarfdump=None):
    """Extract struct layouts from WASM object via llvm-dwarfdump.

    WASM embeds DWARF as custom sections that pyelftools can't read.
    llvm-dwarfdump handles both ELF and WASM natively.

    struct_names: set/list of names to extract, or None for all.
    """
    cmd = [llvm_dwarfdump or "llvm-dwarfdump", "--debug-info", obj_path]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=30)
    except FileNotFoundError:
        print(f"  llvm-dwarfdump not found: {cmd[0]}")
        return {}
    if result.returncode != 0:
        print(f"  llvm-dwarfdump failed: {result.stderr.strip()}")
        return {}

    wanted = set(struct_names) if struct_names else None
    return _parse_llvm_dwarf_output(result.stdout, wanted)


def _parse_llvm_dwarf_output(output, wanted):
    """Parse llvm-dwarfdump --debug-info text for struct layouts.

    Two passes over the output:
      Pass 1: Build type resolution maps (offset → byte_size, offset → ref)
              so we can follow typedef chains to find member sizes.
      Pass 2: Find DW_TAG_structure_type entries we care about, collect
              their DW_TAG_member children with offsets and sizes.
    """
    layouts = {}

    # ── Pass 1: Build type maps for size resolution ──
    # DWARF types form reference chains:
    #   DW_TAG_member → DW_AT_type(0x50) → DW_TAG_typedef "uint64_t"
    #     → DW_AT_type(0x6d) → DW_TAG_base_type "unsigned long long" (sz=8)
    # We need three maps to follow this chain:
    #   type_sizes[offset] = byte_size   (for types that have DW_AT_byte_size)
    #   type_refs[offset]  = ref_offset  (for typedefs that point to another type)
    #   type_tags[offset]  = tag_name    (to detect if a type is a struct)
    type_sizes = {}
    type_refs = {}
    type_tags = {}
    die_pattern = re.compile(
        r'^(0x[0-9a-f]+):\s+DW_TAG_(\w+)\n'
        r'((?:\s+DW_AT_\w+\t.*\n)*)',
        re.MULTILINE
    )
    for m in die_pattern.finditer(output):
        offset = int(m.group(1), 16)
        tag = m.group(2)
        attrs = m.group(3)
        # Track tag for nested struct detection
        type_tags[offset] = tag
        sz_m = re.search(r'DW_AT_byte_size\s*\((\S+)\)', attrs)
        if sz_m:
            type_sizes[offset] = int(sz_m.group(1), 0)
        ref_m = re.search(r'DW_AT_type\s*\((0x[0-9a-f]+)', attrs)
        if ref_m:
            type_refs[offset] = int(ref_m.group(1), 16)

    def resolve_size(ref):
        """Follow type reference chain to find byte_size."""
        for _ in range(10):  # depth limit to prevent infinite loops
            if ref in type_sizes:
                return type_sizes[ref]
            if ref in type_refs:
                ref = type_refs[ref]
            else:
                return 0
        return 0

    # ── Pass 2: Find target structs and their members ──
    current_struct = None
    current_members = []
    current_size = 0

    for m in die_pattern.finditer(output):
        tag = m.group(2)
        attrs = m.group(3)

        # Note: die_pattern captures tag WITHOUT the "DW_TAG_" prefix
        if tag == 'structure_type':
            # Save previous struct if it was one we wanted
            if current_struct and (wanted is None or current_struct in wanted):
                layouts[current_struct] = {
                    "size": current_size, "members": current_members}

            current_struct = None
            current_members = []
            current_size = 0

            name_m = re.search(r'DW_AT_name\s*\("([^"]+)"\)', attrs)
            if name_m:
                sname = name_m.group(1)
                if sname not in layouts and (wanted is None or sname in wanted):
                    current_struct = sname
                    sz_m = re.search(r'DW_AT_byte_size\s*\((\S+)\)', attrs)
                    if sz_m:
                        current_size = int(sz_m.group(1), 0)

        elif tag == 'member' and current_struct:
            # Extract member name, offset, type name, and size
            name_m = re.search(r'DW_AT_name\s*\("([^"]+)"\)', attrs)
            mname = name_m.group(1) if name_m else ""

            off_m = re.search(r'DW_AT_data_member_location\s*\((\S+)\)',
                              attrs)
            moffset = int(off_m.group(1), 0) if off_m else 0

            # Type name from DWARF: DW_AT_type(0x.. "name")
            type_m = re.search(
                r'DW_AT_type\s*\(0x[0-9a-f]+\s+"([^"]+)"\)', attrs)
            mtype = type_m.group(1) if type_m else ""

            # Size and nested struct detection: follow the type reference chain
            msize = 0
            mis_struct = False
            ref_m = re.search(r'DW_AT_type\s*\((0x[0-9a-f]+)', attrs)
            if ref_m:
                ref = int(ref_m.group(1), 16)
                msize = resolve_size(ref)
                # Follow refs to check if the base type is a struct
                r = ref
                for _ in range(10):
                    if type_tags.get(r) == 'structure_type':
                        mis_struct = True
                        break
                    if r in type_refs:
                        r = type_refs[r]
                    else:
                        break

            current_members.append({"name": mname, "offset": moffset,
                                    "size": msize, "type": mtype,
                                    "is_struct": mis_struct})

        elif tag != 'member':
            # Any non-member tag ends the current struct's member list
            if current_struct and (wanted is None or current_struct in wanted):
                layouts[current_struct] = {
                    "size": current_size, "members": current_members}
            current_struct = None

    # Don't forget the last struct if file ends inside one
    if current_struct and (wanted is None or current_struct in wanted):
        layouts[current_struct] = {
            "size": current_size, "members": current_members}

    return layouts


# ═══════════════════════════════════════════════════════════════════════
# Phase 3: Compare layouts and report
# ═══════════════════════════════════════════════════════════════════════


def _suggest_fix(field_name, wasm_member, native_member):
    """Generate a human-readable fix suggestion for one mismatched field.

    Distinguishes between:
      - Size mismatch (e.g., enum: 4B wasm vs 1B native with -fshort-enums)
      - Alignment mismatch (e.g., uint64_t: 8-byte wasm vs 4-byte native)
      - Cascading offset shift (caused by an earlier field's mismatch)
    """
    w_sz = wasm_member.get("size", 0)
    n_sz = native_member.get("size", 0)
    w_off = wasm_member.get("offset", 0)
    n_off = native_member.get("offset", 0)
    type_name = wasm_member.get("type", "") or native_member.get("type", "")

    is_nested = wasm_member.get("is_struct") or native_member.get("is_struct")

    if n_sz != w_sz:
        if is_nested:
            return (f"'{field_name}' (struct {type_name}): inner struct size "
                    f"differs ({n_sz}B native vs {w_sz}B wasm32) "
                    f"— fix the inner struct's alignment first")
        if w_sz == 4 and n_sz == 1:
            return (f"'{field_name}': enum is {n_sz}B native vs {w_sz}B "
                    f"wasm32 — replace with uint32_t")
        return (f"'{field_name}': size differs ({n_sz}B native vs "
                f"{w_sz}B wasm32) — use uint{w_sz * 8}_t")

    if n_off != w_off:
        if w_sz == 8:
            # 8-byte type with wrong offset = alignment difference
            return (f"'{field_name}' ({type_name}): alignment differs"
                    f" — add __attribute__((aligned(8)))")
        # Smaller type at wrong offset = pushed by earlier mismatch
        return (f"'{field_name}' ({type_name}): offset shifted"
                f" (native={n_off} vs wasm={w_off})"
                f" — caused by earlier field alignment mismatch")

    return None


def compare_layouts(struct_name, native_layout, wasm_layout, verbose=False):
    """Compare native vs wasm struct layout, field by field.

    verbose=True:  full table with all fields (OK and MISMATCH)
    verbose=False: only mismatches, suggestions, and summary

    Returns (mismatch_count, suggestions_list, nested_struct_names).
    nested_struct_names contains type names of any members that are structs,
    so the caller can recursively check them.
    """
    mismatches = 0
    suggestions = []
    mismatch_lines = []

    if verbose:
        print(f"\n=== struct {struct_name} ===\n")
        print(f"  {'Field':<16} {'Native':<18} {'WASM':<18} Match")
        print(f"  {'─' * 16} {'─' * 18} {'─' * 18} ─────")

    wasm_by_name = {m["name"]: m for m in wasm_layout["members"]}

    for nm in native_layout["members"]:
        name = nm["name"]
        n_str = f"off={nm['offset']:<3} sz={nm['size']}"

        if name in wasm_by_name:
            wm = wasm_by_name[name]
            w_str = f"off={wm['offset']:<3} sz={wm['size']}"

            if nm["offset"] == wm["offset"] and nm["size"] == wm["size"]:
                if verbose:
                    print(f"  {name:<16} {n_str:<18} {w_str:<18} OK")
            else:
                parts = []
                if nm["offset"] != wm["offset"]:
                    parts.append("offset")
                if nm["size"] != wm["size"]:
                    parts.append("size")
                status = f"MISMATCH ({', '.join(parts)})"
                mismatches += 1
                line = f"  {name:<16} {n_str:<18} {w_str:<18} {status}"
                if verbose:
                    print(line)
                else:
                    mismatch_lines.append(line)
                suggestion = _suggest_fix(name, wm, nm)
                if suggestion:
                    suggestions.append(suggestion)
        else:
            line = (f"  {name:<16} {n_str:<18} "
                    f"{'(missing in WASM)':<18} MISMATCH")
            mismatches += 1
            if verbose:
                print(line)
            else:
                mismatch_lines.append(line)

    # Check for fields in WASM but not native (unlikely but possible)
    native_names = {m["name"] for m in native_layout["members"]}
    for wm in wasm_layout["members"]:
        if wm["name"] not in native_names:
            line = (f"  {wm['name']:<16} {'(missing)':<18} "
                    f"off={wm['offset']:<3} sz={wm['size']:<8} MISMATCH")
            mismatches += 1
            if verbose:
                print(line)
            else:
                mismatch_lines.append(line)

    # Compare total struct size
    n_total = native_layout["size"]
    w_total = wasm_layout["size"]
    if n_total != w_total:
        mismatches += 1
    if verbose:
        sz_match = "OK" if n_total == w_total else "MISMATCH"
        print(f"  {'sizeof':<16} {str(n_total):<18} "
              f"{str(w_total):<18} {sz_match}")

    # Print result
    result = ("PASS" if mismatches == 0
              else f"FAIL — {mismatches} mismatch(es)")

    if verbose:
        print(f"\n  RESULT: {result}")
    elif mismatches > 0:
        print(f"\n  struct {struct_name}: {result}")
        if n_total != w_total:
            print(f"    sizeof: native={n_total} vs wasm={w_total}")
        for line in mismatch_lines:
            print(f"  {line}")
    else:
        print(f"  struct {struct_name}: PASS")

    if suggestions:
        print(f"\n  Suggested fixes for struct {struct_name}:")
        for s in suggestions:
            print(f"    - {s}")
        print(f"    - Add __attribute__((aligned(8))) to the struct itself")

    # Collect names of nested structs so caller can check them recursively
    nested_structs = set()
    for m in native_layout["members"]:
        if m.get("is_struct") and m.get("type"):
            nested_structs.add(m["type"])
    for m in wasm_layout["members"]:
        if m.get("is_struct") and m.get("type"):
            nested_structs.add(m["type"])

    return mismatches, suggestions, nested_structs


# ═══════════════════════════════════════════════════════════════════════
# CLI entry point
# ═══════════════════════════════════════════════════════════════════════


def main():
    parser = argparse.ArgumentParser(
        description="Check WASM-host struct layout consistency")
    parser.add_argument(
        "--source", required=True, nargs="+",
        help="C source file(s) containing NativeSymbol arrays")
    parser.add_argument(
        "--include-dir", action="append", default=[],
        help="Additional include directories (repeatable)")
    parser.add_argument(
        "--native-cc", required=True,
        help="Native C compiler path")
    parser.add_argument(
        "--native-flags", default="",
        help="Extra native compiler flags (e.g. '-m32 -fshort-enums')")
    parser.add_argument(
        "--wasi-sdk", required=True,
        help="Path to wasi-sdk installation")
    parser.add_argument(
        "--wasm-flags", default="",
        help="Extra wasm compiler flags")
    parser.add_argument(
        "--structs",
        help="Override auto-discovery: comma-separated struct names")
    parser.add_argument(
        "--verbose", action="store_true",
        help="Print full field-by-field comparison table")
    parser.add_argument(
        "--quiet", action="store_true",
        help="Suppress progress messages, only print results")
    args = parser.parse_args()

    verbose = args.verbose
    quiet = args.quiet

    wasm_clang = os.path.join(args.wasi_sdk, "bin", "clang")
    if not os.path.isfile(wasm_clang):
        print(f"Error: wasi-sdk clang not found at {wasm_clang}")
        sys.exit(2)

    # ── Phase 1: Discover structs ──
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

    # Manual override: check only specified struct names
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
        sys.exit(0)

    # Collect headers and auto-discover their parent dirs as include paths
    headers = []
    include_dirs = set()
    for s in all_structs:
        if s["header_path"] and s["header_path"] not in headers:
            headers.append(s["header_path"])
            include_dirs.add(os.path.dirname(s["header_path"]))
    include_dirs = list(include_dirs) + args.include_dir

    # ── Phase 2: Compile probes ──
    probe_src = generate_probe_source(all_structs, extra_includes=headers)
    if verbose:
        print(f"\n--- Probe source ---\n{probe_src}---")

    struct_names = [s["struct_name"] for s in all_structs]

    # Build compiler flags: user flags + auto-discovered include dirs
    native_flags = (args.native_flags.split() if args.native_flags else [])
    wasm_flags = ["--target=wasm32"] + (
        args.wasm_flags.split() if args.wasm_flags else [])
    for d in include_dirs:
        native_flags.extend(["-I", d])
        wasm_flags.extend(["-I", d])

    total_mismatches = 0
    all_suggestions = []

    with tempfile.TemporaryDirectory() as tmpdir:
        native_obj = os.path.join(tmpdir, "probe_native.o")
        wasm_obj = os.path.join(tmpdir, "probe_wasm.o")

        if not quiet:
            print(f"\nCompiling probe for native ({args.native_cc})...")
        if not compile_probe(probe_src, native_obj, args.native_cc,
                             native_flags):
            sys.exit(2)

        if not quiet:
            print(f"Compiling probe for wasm32 ({wasm_clang})...")
        if not compile_probe(probe_src, wasm_obj, wasm_clang, wasm_flags):
            sys.exit(2)

        # ── Phase 3: Extract DWARF and compare ──
        llvm_dwarfdump = os.path.join(args.wasi_sdk, "bin", "llvm-dwarfdump")
        if not os.path.isfile(llvm_dwarfdump):
            llvm_dwarfdump = "llvm-dwarfdump"

        # Extract layouts for all structs in the DWARF (pass None to get all)
        # This way nested structs are available without re-extraction
        native_layouts = extract_struct_layouts(native_obj, None)
        wasm_layouts = extract_struct_layouts(wasm_obj, None,
                                              llvm_dwarfdump)

        # Recursive check: start with discovered structs, add nested ones
        checked = set()
        to_check = list(struct_names)

        while to_check:
            name = to_check.pop(0)
            if name in checked:
                continue
            checked.add(name)

            if name not in native_layouts:
                if name in struct_names:
                    # Only warn for top-level structs (not nested ones that
                    # might legitimately not exist in one compilation)
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

            nested_label = ""
            if name not in struct_names:
                nested_label = " (nested)"
            if not quiet and nested_label:
                print(f"  Also checking nested struct: {name}")

            count, suggestions, nested = compare_layouts(
                name, native_layouts[name], wasm_layouts[name],
                verbose=verbose)
            total_mismatches += count
            all_suggestions.extend(suggestions)

            # Queue nested structs for checking
            for ns in nested:
                if ns not in checked:
                    to_check.append(ns)

        total_checked = len(checked)

    # Report unchecked pointer parameters (void*, char*, uint8_t*, etc.)
    # These can't be verified because the cast target is only known at runtime.
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

    print(f"\nSummary: {total_checked} struct(s) checked "
          f"({len(struct_names)} top-level + "
          f"{total_checked - len(struct_names)} nested), "
          f"{total_mismatches} mismatch(es)")

    # Machine-readable block for CMake to extract
    if all_suggestions:
        print("\nFIX_SUGGESTIONS_BEGIN")
        for s in all_suggestions:
            print(f"  - {s}")
        print("FIX_SUGGESTIONS_END")

    sys.exit(0 if total_mismatches == 0 else 1)


if __name__ == "__main__":
    main()

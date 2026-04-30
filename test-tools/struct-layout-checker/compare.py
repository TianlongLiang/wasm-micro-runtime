"""Phase 3: Compare struct layouts and report mismatches.

Compares native vs wasm32 layouts field-by-field, generates fix
suggestions, handles recursive nested struct checking, and formats
output with machine-readable markers for CMake.
"""


# Classify the root cause of a field mismatch and generate a human-readable
# fix suggestion. The classification order matters:
#   1. Size mismatch first (enum or nested struct — these are the root cause)
#   2. Offset mismatch second (often a cascading effect from an earlier field)
# If both offset and size differ, the size mismatch is the more actionable fix.
def suggest_fix(field_name, wasm_member, native_member):
    """Generate a fix suggestion for one mismatched field.

    Returns a string, or None if no mismatch.
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
            return (f"'{field_name}' ({type_name}): alignment differs"
                    f" — add __attribute__((aligned(8)))")
        return (f"'{field_name}' ({type_name}): offset shifted"
                f" (native={n_off} vs wasm={w_off})"
                f" — caused by earlier field alignment mismatch")

    return None


# Compare two struct layouts field-by-field. For each field present in the
# native layout, look it up by name in the wasm layout and compare offset
# and size. Also check for fields missing on either side and total struct size.
#
# The function collects names of nested struct members so the caller can
# recursively check those inner structs too.
#
# Output format includes machine-readable markers (FIX_SUGGESTIONS_BEGIN/END)
# that check_struct_layout.cmake parses to extract fix suggestions.
def compare_layouts(struct_name, native_layout, wasm_layout, verbose=False):
    """Compare native vs wasm struct layout, field by field.

    Returns (mismatch_count, suggestions_list, nested_struct_names).
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
                suggestion = suggest_fix(name, wm, nm)
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

    n_total = native_layout["size"]
    w_total = wasm_layout["size"]
    if n_total != w_total:
        mismatches += 1
    if verbose:
        sz_match = "OK" if n_total == w_total else "MISMATCH"
        print(f"  {'sizeof':<16} {str(n_total):<18} "
              f"{str(w_total):<18} {sz_match}")

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

    nested_structs = set()
    for m in native_layout["members"]:
        if m.get("is_struct") and m.get("type"):
            nested_structs.add(m["type"])
    for m in wasm_layout["members"]:
        if m.get("is_struct") and m.get("type"):
            nested_structs.add(m["type"])

    return mismatches, suggestions, nested_structs

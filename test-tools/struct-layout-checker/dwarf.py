"""DWARF struct layout extraction from ELF and WASM object files.

Auto-detects format:
  - ELF (.o, .so, executables) -> pyelftools
  - WASM (.wasm, .o) -> llvm-dwarfdump
"""
import os
import re
import subprocess


# Auto-detect file format by reading the first 4 bytes (magic number):
#   b'\x00asm' = WebAssembly module
#   b'\x7fELF' = ELF object/executable/shared library
# Each format requires a different parser for DWARF extraction.
def extract_struct_layouts(obj_path, struct_names=None, llvm_dwarfdump=None):
    """Extract struct layouts from DWARF in an object file.

    Returns: {"struct_name": {"size": N, "members": [...]}, ...}
    """
    with open(obj_path, 'rb') as f:
        magic = f.read(4)

    if magic[:4] == bytes([0x00, 0x61, 0x73, 0x6d]):
        return _extract_layouts_llvm(obj_path, struct_names, llvm_dwarfdump)
    return _extract_layouts_elf(obj_path, struct_names)


def llvm_dwarfdump_or_default(explicit_path):
    """Find llvm-dwarfdump: explicit path, WASI_SDK, or PATH."""
    if explicit_path and os.path.isfile(explicit_path):
        return explicit_path
    wasi_sdk = os.environ.get("WASI_SDK_PATH", "")
    if wasi_sdk:
        candidate = os.path.join(wasi_sdk, "bin", "llvm-dwarfdump")
        if os.path.isfile(candidate):
            return candidate
    return "llvm-dwarfdump"


def _extract_layouts_elf(obj_path, struct_names=None):
    """Extract struct layouts from ELF via pyelftools."""
    from elftools.elf.elffile import ELFFile

    layouts = {}
    wanted = set(struct_names) if struct_names else None

    with open(obj_path, 'rb') as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info():
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

                struct_size = 0
                if 'DW_AT_byte_size' in die.attributes:
                    struct_size = die.attributes['DW_AT_byte_size'].value

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
                            moffset = loc.value[-1] if loc.value else 0

                    msize, mtype, mis_struct = _resolve_type_elf(
                        dwarf, cu, child)
                    members.append({"name": mname, "offset": moffset,
                                    "size": msize, "type": mtype,
                                    "is_struct": mis_struct})

                layouts[name] = {"size": struct_size, "members": members}

    return layouts


# DWARF doesn't store member sizes directly on DW_TAG_member.
# Instead, each member has DW_AT_type pointing to a type DIE.
# Types form reference chains:
#   DW_TAG_member -> DW_TAG_typedef "uint64_t" -> DW_TAG_base_type "unsigned long long" (sz=8)
# We follow this chain until we find DW_AT_byte_size.
# If the resolved type is DW_TAG_structure_type, the member is a nested struct.
def _resolve_type_elf(dwarf, cu, member_die):
    """Follow DW_AT_type chain to find size and type name."""
    if 'DW_AT_type' not in member_die.attributes:
        return 0, "", False

    type_offset = member_die.attributes['DW_AT_type'].value

    # DWARF type references can be either absolute offsets or
    # CU-relative offsets depending on the DWARF version and producer.
    # Try both interpretations to handle both cases.
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

    type_name = ""
    if 'DW_AT_name' in type_die.attributes:
        type_name = type_die.attributes['DW_AT_name'].value.decode('utf-8')

    while type_die and type_die.tag in (
        'DW_TAG_typedef', 'DW_TAG_const_type',
        'DW_TAG_volatile_type', 'DW_TAG_restrict_type',
    ):
        if 'DW_AT_type' not in type_die.attributes:
            break
        type_die = get_die(type_die.attributes['DW_AT_type'].value)

    is_struct = (type_die is not None
                 and type_die.tag == 'DW_TAG_structure_type')

    if is_struct and 'DW_AT_name' in type_die.attributes:
        type_name = type_die.attributes['DW_AT_name'].value.decode('utf-8')

    if type_die and 'DW_AT_byte_size' in type_die.attributes:
        return type_die.attributes['DW_AT_byte_size'].value, type_name, is_struct
    return 0, type_name, is_struct


def _extract_layouts_llvm(obj_path, struct_names=None, llvm_dwarfdump=None):
    """Extract struct layouts from WASM via llvm-dwarfdump."""
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
    return parse_llvm_dwarf_output(result.stdout, wanted)


# Parse the text output of `llvm-dwarfdump --debug-info`.
# We can't use pyelftools for WASM because WASM embeds DWARF in custom
# sections that pyelftools doesn't understand. llvm-dwarfdump handles
# both ELF and WASM natively, so we parse its text output instead.
#
# Two-pass approach:
#   Pass 1: Build maps (offset -> size, offset -> type_ref, offset -> tag)
#           for resolving typedef chains to find member byte sizes.
#   Pass 2: Walk DW_TAG_structure_type entries, collect their DW_TAG_member
#           children with offsets and sizes resolved via the Pass 1 maps.
def parse_llvm_dwarf_output(output, wanted):
    """Parse llvm-dwarfdump --debug-info text for struct layouts.

    Public for unit testing with hardcoded output strings.
    """
    layouts = {}

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
        type_tags[offset] = tag
        sz_m = re.search(r'DW_AT_byte_size\s*\((\S+)\)', attrs)
        if sz_m:
            type_sizes[offset] = int(sz_m.group(1), 0)
        ref_m = re.search(r'DW_AT_type\s*\((0x[0-9a-f]+)', attrs)
        if ref_m:
            type_refs[offset] = int(ref_m.group(1), 16)

    # Follow the type reference chain up to 10 levels deep.
    # Most chains are 1-2 levels (e.g., uint64_t -> unsigned long long),
    # but const/volatile qualifiers can add extra indirection.
    # The depth limit prevents infinite loops from malformed DWARF.
    def resolve_size(ref):
        for _ in range(10):
            if ref in type_sizes:
                return type_sizes[ref]
            if ref in type_refs:
                ref = type_refs[ref]
            else:
                return 0
        return 0

    current_struct = None
    current_members = []
    current_size = 0

    # State machine: when we see a DW_TAG_structure_type, we start
    # collecting its DW_TAG_member children. Any non-member tag (or a
    # new structure_type) ends the current struct's member list.
    for m in die_pattern.finditer(output):
        tag = m.group(2)
        attrs = m.group(3)

        if tag == 'structure_type':
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
            name_m = re.search(r'DW_AT_name\s*\("([^"]+)"\)', attrs)
            mname = name_m.group(1) if name_m else ""

            off_m = re.search(r'DW_AT_data_member_location\s*\((\S+)\)',
                              attrs)
            moffset = int(off_m.group(1), 0) if off_m else 0

            type_m = re.search(
                r'DW_AT_type\s*\(0x[0-9a-f]+\s+"([^"]+)"\)', attrs)
            mtype = type_m.group(1) if type_m else ""

            msize = 0
            mis_struct = False
            ref_m = re.search(r'DW_AT_type\s*\((0x[0-9a-f]+)', attrs)
            if ref_m:
                ref = int(ref_m.group(1), 16)
                msize = resolve_size(ref)
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
            if current_struct and (wanted is None or current_struct in wanted):
                layouts[current_struct] = {
                    "size": current_size, "members": current_members}
            current_struct = None

    if current_struct and (wanted is None or current_struct in wanted):
        layouts[current_struct] = {
            "size": current_size, "members": current_members}

    return layouts

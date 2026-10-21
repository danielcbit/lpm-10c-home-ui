#!/usr/bin/env python3
"""Check whether a flash range in a built image is unreachable, so it can be reused.

Redirecting a vendor call leaves the old routine stranded. Its bytes are inside
the update file, so reusing them is the only way to add code once the file has
reached the vendor size limit (docs/lpm-10c-bootloader-analysis.md).

Overwriting live code or data would brick the application or corrupt the screen,
so a range is reported dead only when nothing outside the dead ranges refers to it:
  - no BL or B.W branch,
  - no 16-bit conditional or unconditional branch,
  - no 32-bit word holding an address inside it (for code, a Thumb pointer with bit 0 set),
  - no PC-relative load that lands inside it,
  - no address computed from the PC (ADR, ADR.W, or a literal added to PC) inside it,
  - its guard, if it has one, holds: the patch that strands it is really there,
  - and a routine that could still index into it is called only from reviewed sites.

For data the question is whether any code depends on the bytes' values. Reading
flash cannot fault, so a data range is dead when nothing outside dead code forms an
address inside it: its only readers are disabled. An out-of-bounds index into some
neighbouring table could still reach it, but that already reads meaningless bytes.

A reference from inside another dead range is harmless only if that range is dead
too, so the dead set is computed as a fixpoint: a range with any other reference
drops out, which can make references from it count against the rest.

Thumb-2 mixes code and data, so a linear scan reports matches that are really
halves of 32-bit instructions or literal pools. Branches are kept regardless,
because a spurious one only costs a range that could have been reused. A
PC-relative load counts only when the word it reads looks like a literal.

  python3 scripts/check_dead_code.py firmware/patched/home-ui/LPM-10C_TX_V2.0.7_260907.bin

Exit status is 0 when every range is unreachable. This cannot see computed jumps
through a table or a function pointer built at run time, so treat a pass as
strong evidence, not proof, and keep a known-good image for recovery.
"""
import argparse
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x08009000
CODE_START, CODE_END = 0x1000, 0x6935C  # file offsets: after the vector table, up to the scatter table
VENDOR = ROOT / 'firmware' / 'extracted' / 'LPM-10C' / 'LPM-10C_V2.0.7' / 'LPM-10C_TX_V2.0.7_260907.bin'
# The one reference kind the patches create on purpose: a vendor BL redirected to added code in a range.
PATCHABLE_KINDS = ('branch',)

# Ranges stranded by scripts/build_patched_image.py, as flash addresses.
# The menu draw really ends at 0x0800F904, but the tail is left out: a load at 0x0800F424 appears
# to read a literal at 0x0800F808, and although that offset is provably the second half of the
# "push.w" at 0x0800F422, the range stops just below that word rather than relying on the judgement.
# The ASCII glyph drawer 0x08018A34 indexes its fonts with (c - 0x20) & 0xFF and no range check,
# so a character outside 0x20-0x7E would read the CJK table up to 0x08071307. No caller passes one
# (see CALLERS), so the whole table is reused.
DEAD_RANGES = {
    'grid menu draw (replaced by lpm_home_draw)': (0x0800F648, 0x0800F808),
    'grid navigation (replaced by lpm_home_key)': (0x080180C0, 0x080181AA),
    'CJK glyph blitter body (entry patched to bx lr)': (0x08018DB4, 0x08018F0C),
    'CJK glyph table (blitter patched to bx lr)': (0x080708F8, 0x08071E58),
    # Pictures nothing draws any more (docs/lpm-10c-bootloader-analysis.md, "Pictures"). Each one is
    # an 8-byte header (width at +2, height at +4) and RGB565 pixels, reached only through the getter
    # 0x0800E8E0, which reads a table initialised at boot. The four blocks are whole pictures.
    'pictures 4, 0 and 2 (grid icons)': (0x08026B34, 0x0802A8FC),
    'pictures 8 and 3 (grid icons)': (0x0802C4AC, 0x0802EAD0),
    'pictures 21, 7 and 6 (warning title, grid icons)': (0x0802FA09, 0x08031EBD),
    'picture 1 (grid icon)': (0x080323D1, 0x08034359),
    # Last, because it holds the added code: the tests look for the redirected BLs into it.
    'pictures 9, 5, 22 and 23 (grid icons, company name, QC test)': (0x080584F9, 0x0806E2A7),
}
# Ranges holding data rather than code, with the whole object they belong to. Code reaches a table
# by forming its base and adding an index, so no live code may form an address anywhere in the
# object, even where only part of it is reused; and any word pointing into it counts.
DATA_RANGES = {name: bounds for name, bounds in DEAD_RANGES.items() if name.startswith(('CJK glyph table', 'picture'))}
# The picture getter's tbb table (0x0800E8EA, one byte per index): the freed pictures branch to
# "movs r0, #0" (byte 0x28), and the drawer 0x0800F422 returns at once for NULL.
PICTURE_GETTER = (0x0800E8EA, bytes.fromhex('282828282828282828282223161718191a1b1c1d1e282828'),
                  'pictures 0-9 and 21-23 return NULL')
# A range is dead only while the patch that strands it is in the image: (flash address, bytes, what).
GUARDS = {
    'CJK glyph blitter body (entry patched to bx lr)': (0x08018DB0, bytes.fromhex('7047 00bf'), 'bx lr; nop'),
    'CJK glyph table (blitter patched to bx lr)': (0x08018DB0, bytes.fromhex('7047 00bf'), 'bx lr; nop'),
    **{name: PICTURE_GETTER for name in DEAD_RANGES if name.startswith('picture')},
}
# A range one more routine could reach through an unchecked index is dead only while every call to
# that routine was reviewed: name -> (routine, the reviewed BL sites, what each one guarantees).
# The ASCII drawer's callers: draw_string 0x08018D48 and the halfword drawer 0x08018C4C stop at
# any character outside 0x20-0x7E; the number drawers 0x08018CB4 and 0x08018F3E pass ' ' or a
# digit; the rest pass a constant ('G', '.', 'V', ':') or draw a fixed count of characters from
# buffers formatted with "%03d.%03d.%03d.%03d" or "%03d%03d", which are digits and dots only.
CALLERS = {
    'CJK glyph table (blitter patched to bx lr)': (0x08018A35 & ~1, frozenset({
        0x0800D708, 0x0800D73E, 0x08013A00, 0x08013A3C, 0x08013CBC, 0x08018C9E, 0x08018D0E, 0x08018D34,
        0x08018D8A, 0x08018FA0, 0x08018FC0, 0x08018FE8, 0x0801B7AE, 0x0801BD62, 0x0801DF70, 0x0801E346,
        0x0801E36C}), 'a character in 0x20-0x7E'),
}
# Bytes that are never executed, so instruction-like patterns in them are not references:
# (start, end, what). Nothing branches here; the glyph drawers read these as bitmaps.
NOT_CODE = (
    (0x0806FE94, 0x080708F8, 'ASCII 6x12 and 8x16 glyphs'),
    # After 0x0800F4B4 returns (pop at 0x0800F5CA): two pool words and the log string
    # "%d GUI: APP_GUI_Show_String ...". Its text decodes as "ldr r1, [pc, #0x154]" into the grid menu draw.
    (0x0800F5D0, 0x0800F608, 'literal pool and log string of 0x0800F4B4'),
    # After 0x08017E74 returns (pop at 0x08017F8A): pool words at 0x08017F90/F94/FB4 and the log
    # strings "%d PWR_Off by reason %d" (adr at 0x08017ED2) and "%d Charge by not PWR_ON %d" (adr at
    # 0x08017F64). The second string's text decodes as "ldr r7, [pc, #0x17c]" into the grid navigation.
    (0x08017F90, 0x08017FD8, 'literal pool and log strings of 0x08017E74'),
    # All 24 pictures, back to back: pixels, never executed. 0x0802EAD0 is the one byte between
    # pictures 3 and 15, where entry 14 of the table points; nothing draws index 14.
    (0x08026712, 0x0806F2D9, 'RGB565 pictures'),
)

# Words that look like addresses in a data range but are instructions, each reviewed in context and
# kept only while its bytes are unchanged: flash address -> (bytes, the instruction).
INSTRUCTION_WORDS = {
    0x0801EE10: (bytes.fromhex('90f80508'), 'ldrb.w r0, [r0, #0x805] (reads 0x0805F890 as a word)'),
}

# Offsets that decode as a BL or B.W but lie inside another instruction, each confirmed by decoding
# the enclosing function from its start, and kept only while the bytes are unchanged. Branches are
# otherwise never filtered (see references_from). Both are in the soft-float shift helper whose
# function starts at 0x08025B3C: flash address -> (bytes, why).
NOT_BRANCHES = {
    0x08025C08: (bytes.fromhex('02f001fa'), 'second half of lsr.w r0, r1, r2 at 0x08025C06'),
    0x08025C4A: (bytes.fromhex('02f021fa'), 'second half of lsr.w r0, r0, r2 at 0x08025C48'),
}


def function_start(data, offset, limit=0x800):
    """Walk back to the nearest push prologue, the usual start of a Thumb function."""
    for probe in range(offset, max(CODE_START, offset - limit), -2):
        half = struct.unpack_from('<H', data, probe)[0]
        if (half & 0xFF00) == 0xB500 or half == 0xE92D:
            return probe
    return None


def is_instruction_boundary(data, offset):
    """True when a linear decode from the enclosing function lands exactly on offset."""
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    start = function_start(data, offset)
    if start is None:
        return False
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for instruction in md.disasm(data[start:offset + 4], start + BASE):
        if instruction.address - BASE == offset:
            return True
        if instruction.address - BASE > offset:
            return False
    return False


def looks_like_literal(word):
    """A literal pool holds pointers or small constants, not instruction encodings."""
    return (0x20000000 <= word < 0x20030000 or 0x08000000 <= word < 0x08080000
            or 0x40000000 <= word < 0x60100000 or word < 0x10000)


def branch_target(data, offset):
    """Flash address of a BL or B.W at offset, or None."""
    first, second = struct.unpack_from('<HH', data, offset)
    if (first >> 11) != 0b11110 or (second >> 14) != 0b11:
        return None
    sign = (first >> 10) & 1
    i1 = 1 - (((second >> 13) & 1) ^ sign)
    i2 = 1 - (((second >> 11) & 1) ^ sign)
    imm = (sign << 24) | (i1 << 23) | (i2 << 22) | ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
    if sign:
        imm -= 1 << 25
    return offset + BASE + 4 + imm


def short_branch_target(data, offset):
    """Flash address of a 16-bit B or B<cond> at offset, or None."""
    half = struct.unpack_from('<H', data, offset)[0]
    if (half & 0xF800) == 0xE000:
        imm = half & 0x7FF
        return offset + BASE + 4 + (imm - 0x800 if imm & 0x400 else imm) * 2
    if (half & 0xF000) == 0xD000 and (half & 0x0F00) != 0x0F00:
        imm = half & 0xFF
        return offset + BASE + 4 + (imm - 0x100 if imm & 0x80 else imm) * 2
    return None


def pc_load_target(data, offset):
    """Flash address a PC-relative load at offset reads, or None."""
    half = struct.unpack_from('<H', data, offset)[0]
    aligned = ((offset + BASE + 4) & ~3)
    if (half >> 11) == 0b01001:
        return aligned + (half & 0xFF) * 4
    if (half & 0xFF7F) == 0xF85F or half == 0xF8DF:
        imm = struct.unpack_from('<H', data, offset + 2)[0] & 0xFFF
        return aligned + (imm if (half & 0x80 or half == 0xF8DF) else -imm)
    return None


def pc_relative_addresses(data):
    """Addresses the code computes from the PC, as (flash address of the instruction, address)."""
    out = []
    for offset in range(CODE_START, CODE_END - 4, 2):
        half = struct.unpack_from('<H', data, offset)[0]
        here = offset + BASE
        aligned = (here + 4) & ~3
        if (half & 0xF800) == 0xA000:  # adr rd, #imm8 * 4
            out.append((here, aligned + (half & 0xFF) * 4))
        elif (half & 0xFBFF) in (0xF20F, 0xF2AF):  # adr.w: addw / subw rd, pc, #imm12
            second = struct.unpack_from('<H', data, offset + 2)[0]
            imm = ((half >> 10) & 1) << 11 | ((second >> 12) & 7) << 8 | (second & 0xFF)
            out.append((here, aligned - imm if (half & 0xFBFF) == 0xF2AF else aligned + imm))
        elif (half & 0xFF78) == 0x4478:  # add rd, pc, after "ldr rd, [pc, #imm]"
            rd = (half & 7) | ((half >> 4) & 8)
            for back in range(offset - 2, offset - 16, -2):
                earlier = struct.unpack_from('<H', data, back)[0]
                if earlier >> 11 == 0b01001 and ((earlier >> 8) & 7) == rd:
                    value = (here + 4 + struct.unpack_from('<I', data, ((back + BASE + 4) & ~3) + (earlier & 0xFF) * 4 - BASE)[0]) & 0xFFFFFFFF
                    following = struct.unpack_from('<H', data, offset + 2)[0]
                    if (following & 0xF000) == 0x3000 and ((following >> 8) & 7) == rd:  # adds / subs rd, #imm8
                        value += -(following & 0xFF) if following & 0x0800 else following & 0xFF
                    out.append((here, value))
                    break
    return out


def references_from(data, low, high, data_range, ignore, computed):
    """References into [low, high) from outside the ignored ranges, as (kind, referrer, detail)."""
    def ignored(address):
        return any(lo <= address < hi for lo, hi in ignore)

    found = []
    for offset in range(CODE_START, CODE_END - 4, 2):
        here = offset + BASE
        if ignored(here):
            continue
        # Branches are never filtered. Deciding whether an offset is a real instruction boundary
        # needs a reliable function start, and when that walk goes wrong the filter drops a genuine
        # call: it hid the vendor image's own calls into these ranges. A spurious branch only costs
        # a range that could have been reused, so unfiltered is the safe direction to be wrong in.
        target = branch_target(data, offset)
        known = NOT_BRANCHES.get(here)
        if known and data[offset:offset + 4] == known[0]:
            target = None
        if target is not None and low <= target < high:
            found.append(('branch', here, target))
            continue
        target = short_branch_target(data, offset)
        if target is not None and low <= target < high:
            found.append(('short branch', here, target))
            continue
        target = pc_load_target(data, offset)
        if target is not None and low <= target < high:
            # Judged by what is read, not by where the load is: a literal pool holds pointers and
            # small constants, so a word of instruction bytes means this was not really a load.
            word = struct.unpack_from('<I', data, target - BASE)[0]
            if looks_like_literal(word):
                found.append(('literal load', here, target))
    for offset in range(CODE_START, CODE_END - 4, 4):
        word = struct.unpack_from('<I', data, offset)[0]
        if ignored(offset + BASE):
            continue
        # A stored pointer to a Thumb routine always has bit 0 set, because BX and BLX read it as
        # the instruction set. A word without it cannot call into code, and in this firmware such
        # matches are instruction encodings (for example "ldrb.w r0, [r0, #0x800]"). Data has no
        # such rule, so any word pointing into a data range counts.
        if (data_range or word & 1) and low <= (word & ~1) < high:
            known = INSTRUCTION_WORDS.get(offset + BASE)
            if known and data[offset:offset + 4] == known[0]:
                continue
            found.append(('pointer', offset + BASE, word))
    for here, address in computed:
        if ignored(here):
            continue
        if low <= address < high:
            found.append(('pc-relative address', here, address))
    return found


def guard_failure(data, name):
    """A reference-like entry when the patch that strands the range is missing, else None."""
    if name in GUARDS:
        address, expected, what = GUARDS[name]
        if data[address - BASE:address - BASE + len(expected)] != expected:
            return ('guard', address, int.from_bytes(expected, 'little'))
    if name in CALLERS:
        routine, reviewed, what = CALLERS[name]
        # Added code past the vendor payload is scanned too: a new caller has not been reviewed.
        for offset in range(CODE_START, len(data) - 3, 2):
            if branch_target(data, offset) == routine and offset + BASE not in reviewed:
                return ('unreviewed caller', offset + BASE, routine)
    return None


def patched_here(data, vendor, where):
    """True when the 4 bytes at flash address where differ from the vendor file: a patch put them there."""
    offset = where - BASE
    return vendor is not None and offset + 4 <= len(vendor) and data[offset:offset + 4] != vendor[offset:offset + 4]


def analyse(data, vendor=None):
    """{range name: references}, after removing ranges that are referenced from live code until stable.

    With the vendor image, references at patched sites are dropped: added code placed in a reused
    range is reached through BLs the patches redirected, and those are meant to point there."""
    computed = pc_relative_addresses(data)
    dead = set(DEAD_RANGES)
    while True:
        result = {}
        for name, (low, high) in DEAD_RANGES.items():
            ignore = ([DEAD_RANGES[other] for other in dead if other != name] + [(low, high)]
                      + [(start, end) for start, end, _ in NOT_CODE])
            target = DATA_RANGES.get(name, (low, high))
            refs = [ref for ref in references_from(data, *target, name in DATA_RANGES, ignore, computed)
                    if not (ref[0] in PATCHABLE_KINDS and patched_here(data, vendor, ref[1]))]
            failure = guard_failure(data, name)
            result[name] = ([failure] if failure else []) + refs
        live = {name for name, refs in result.items() if refs}
        if not live & dead:
            return result
        dead -= live


def find_references(data, low, high, vendor=None):
    """Confirmed references into the dead range [low, high), as (kind, flash address, detail)."""
    for name, bounds in DEAD_RANGES.items():
        if bounds == (low, high):
            return analyse(data, vendor)[name]
    return references_from(data, low, high, False, [(low, high)], pc_relative_addresses(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', nargs='?', type=Path,
                        default=ROOT / 'firmware' / 'patched' / 'home-ui' / 'LPM-10C_TX_V2.0.7_260907.bin')
    args = parser.parse_args()
    try:
        import capstone  # noqa: F401
    except ImportError:
        raise SystemExit('Needs Capstone 5: pip install capstone (see README)')

    data = args.image.read_bytes()
    vendor = VENDOR.read_bytes() if VENDOR.exists() else None
    if vendor is None:
        print(f'{VENDOR} not found: BLs the patches redirected into a range count as references')
    failed = False
    results = analyse(data, vendor)
    for name, (low, high) in DEAD_RANGES.items():
        references = results[name]
        status = 'REACHABLE' if references else 'unreachable'
        kind = 'data' if name in DATA_RANGES else 'code'
        print(f'{low:#010x}-{high:#010x}  {high - low:4d} bytes  {kind}  {status}  {name}')
        for ref_kind, where, detail in references[:10]:
            print(f'    {ref_kind} from {where:#010x} -> {detail:#x}')
        failed |= bool(references)
    total = sum(high - low for low, high in DEAD_RANGES.values())
    print(f'\n{total} bytes reusable' if not failed else '\nSome ranges are still referenced; do not reuse them')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

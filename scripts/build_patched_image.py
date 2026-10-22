#!/usr/bin/env python3
"""Build a modified TX update image from the vendor V2.0.7 file by same-length byte patches.

Every patch must match the vendor bytes it replaces and keep its length, so no
address, pointer, header field or size changes. Patches may touch application
code and read-only data, but code patches are limited to single instructions
whose replacement has the same size, or BL calls redirected to added code. A
patch set may also add bytes after the vendor payload; the header length is
then extended to cover them, and the vendor header layout is otherwise kept.
Added C code (src/firmware_patch/) is compiled with clang and copied unlinked,
so it must compile to a single .text section without relocations.
The copy-protection routines, the vector table, the header and the C runtime's
compressed initialisation data are refused outright (docs/lpm-10c-bootloader-analysis.md).

  python3 scripts/build_patched_image.py            # writes firmware/patched/<name>/

The output keeps the vendor file name, because the bootloader's file-name rules
are unknown. Copy it from Windows exactly like a vendor update.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'firmware' / 'extracted' / 'LPM-10C' / 'LPM-10C_V2.0.7' / 'LPM-10C_TX_V2.0.7_260907.bin'
VENDOR_SHA256 = '3bcc50065e16e93ddeff5fb9dce04f1b0447fd3f68a084803070978aef2bcaaa'
BASE = 0x08009000  # flash address = file offset + BASE

# Application code and read-only data, from after the vector table to the scatter-load table at 0x0807235C.
PATCHABLE = (0x1200, 0x6935C)
PAYLOAD_END = 0x6944C  # first file offset after the vendor payload (flash 0x0807244C)
# The update drive accepts at most the vendor file size. A 438,272-byte home-ui image installed only partly:
# data past 434,176 bytes stayed erased and the app faulted on the first icon (docs/lpm-10c-bootloader-analysis.md).
FLASH_LIMIT = 0x6A000  # file offset; added code must end here, i.e. at flash 0x08073000
FILE_ALIGN = 0x1000  # the vendor file is zero-padded to a whole 4 KB after its payload
# Ranges that must never change, as file offsets: (start, end, reason).
PROTECTED = (
    (0x0000, 0x1000, 'update header'),
    (0x1000, 0x1200, 'vector table'),
    (0x7000, 0x7680, 'BootFlag and Cust_Info flash routines'),
    (0xdf00, 0xe008, 'flash driver and V2 marker check'),
    (0x10634, 0x10698, 'info-page writer'),
    (0x1a18c, 0x1a2b0, 'main()'),
    (0x1b63c, 0x1b6a8, 'UID binding check'),
    (0x1ba4, 0x1f80, 'UID cipher'),
    (0x6935C, 0x6944C, 'scatter-load table and compressed RW data'),
)



def thumb_branch(source, target, link):
    """Thumb-2 BL (link) or B.W encoding at file offset source to file offset target."""
    offset = target - (source + 4)
    if offset % 2 or not -(1 << 24) <= offset < (1 << 24):
        raise ValueError(f'branch {source:#x} -> {target:#x} out of range')
    s = (offset >> 24) & 1
    i1, i2 = (offset >> 23) & 1, (offset >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    h1 = 0xF000 | s << 10 | (offset >> 12) & 0x3FF
    h2 = (0xD000 if link else 0x9000) | j1 << 13 | j2 << 11 | (offset >> 1) & 0x7FF
    return h1.to_bytes(2, 'little') + h2.to_bytes(2, 'little')


def extension_hook():
    """Boundary test: code and data after the vendor payload, reached from existing code.

    The log task prints its version line with snprintf (0x0800A3C0) called from 0x080133DE,
    with the format in r2. The hook at the old payload end loads its own format into r2 and
    tail-calls snprintf, so the stack argument and the return address are untouched.
    """
    hook = PAYLOAD_END                                        # flash 0x0807244C, word aligned
    fmt = b'%d Current FW Version: %s +ext\r\n\x00'
    code = (0xA201).to_bytes(2, 'little')                     # adr r2, fmt (pc 0x..450 + 4)
    code += thumb_branch(hook + 2, 0x800A3C0 - BASE, link=False)  # b.w snprintf
    code += b'\x00\x00'                                      # pad so fmt is word aligned
    blob = code + fmt
    blob += bytes(-len(blob) % 4)
    return [
        (0x68FD8, b'Length\x00', b'LENGTH\x00', 'main-menu label, page 1 slot 3 (180,190)'),
        (0xA3DE, thumb_branch(0xA3DE, 0x800A3C0 - BASE, link=True), thumb_branch(0xA3DE, hook, link=True),
         'version log 0x080133DE: bl snprintf -> bl hook at 0x0807244C'),
        (hook, bytes(len(blob)), blob, 'hook: adr r2, fmt; b.w snprintf; format string'),
    ]


def find_clang():
    for candidate in (os.environ.get('CLANG'), '/opt/homebrew/opt/llvm@22/bin/clang', shutil.which('clang')):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit('clang not found: set CLANG or install LLVM (see README)')


def elf_text(obj):
    """Return (.text bytes, {symbol: offset}) of an ELF32 ARM object, or raise if it needs linking."""
    shoff, = struct.unpack_from('<I', obj, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', obj, 0x2E)
    sections = [struct.unpack_from('<IIIIIIIIII', obj, shoff + i * shentsize) for i in range(shnum)]
    names = sections[shstrndx]

    def name(offset, table):
        start = table[4] + offset
        return obj[start:obj.index(b'\0', start)].decode()

    by_name = {name(s[0], names): (i, s) for i, s in enumerate(sections)}
    for section_name, (_, s) in by_name.items():
        allocated = s[2] & 0x2
        if section_name.startswith(('.rel.text', '.rela.text')):
            raise ValueError('added code needs relocations; call vendor code through absolute addresses')
        if allocated and s[5] and section_name not in ('.text', '.ARM.exidx'):
            raise ValueError(f'added code has a {section_name} section; it must be .text only')
    text_index, text = by_name['.text']
    code = obj[text[4]:text[4] + text[5]]
    _, symtab = by_name['.symtab']
    strtab = sections[symtab[6]]
    symbols = {}
    for offset in range(symtab[4], symtab[4] + symtab[5], 16):
        st_name, value, _, info, _, shndx = struct.unpack_from('<IIIBBH', obj, offset)
        if shndx == text_index and info >> 4 == 1 and info & 0xF == 2:  # global function
            symbols[name(st_name, strtab)] = value & ~1
    return code, symbols


def compile_added_code(source):
    """Compile one freestanding C file for the Cortex-M4 and return (.text, symbols)."""
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / 'added.o'
        subprocess.run([find_clang(), '--target=thumbv7em-none-eabi', '-mcpu=cortex-m4', '-mfloat-abi=soft',
                        '-ffreestanding', '-fno-builtin', '-std=c99', '-Os', '-Wall', '-Wextra', '-Werror',
                        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-c', str(source), '-o', str(obj)],
                       check=True)
        return elf_text(obj.read_bytes())


def redirect_call(site, old_target, new_target, note):
    """Patch the BL at file offset site from vendor function old_target (flash) to file offset new_target."""
    return (site, thumb_branch(site, old_target - BASE, link=True), thumb_branch(site, new_target, link=True), note)


def menu_list():
    """List-style main menu: src/firmware_patch/menu_list.c placed after the vendor payload."""
    code, symbols = compile_added_code(ROOT / 'src' / 'firmware_patch' / 'menu_list.c')
    code += bytes(-len(code) % 4)
    draw, nav = PAYLOAD_END + symbols['lpm_menu_draw'], PAYLOAD_END + symbols['lpm_menu_nav']
    return [
        (0x68FD8, b'Length\x00', b'LENGTH\x00', 'main-menu label, page 1 slot 3 (180,190)'),
        redirect_call(0x6A7E, 0x800F648, draw, 'menu entry 0x0800FA7E: grid draw -> lpm_menu_draw'),
        redirect_call(0x6A9E, 0x800F648, draw, 'menu redraw 0x0800FA9E: grid draw -> lpm_menu_draw'),
        redirect_call(0x4802, 0x80180C0, nav, 'menu keys 0x0800D802: grid navigation -> lpm_menu_nav'),
        (PAYLOAD_END, bytes(len(code)), code, f'menu_list.c .text, {len(code)} bytes at 0x0807244C'),
    ]


ARM_CFLAGS = ['--target=thumbv7em-none-eabi', '-mcpu=cortex-m4', '-mfloat-abi=softfp', '-mfpu=fpv4-sp-d16',
              '-ffreestanding', '-fno-builtin', '-std=c99', '-Os', '-ffunction-sections', '-fdata-sections', '-Wall',
              '-Wextra', '-Werror', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']


def find_tool(name):
    """An LLVM tool next to the chosen clang, else on PATH (ld.lld comes from Homebrew lld)."""
    for candidate in (Path(find_clang()).parent / name, shutil.which(name), Path('/opt/homebrew/bin') / name):
        if candidate and Path(candidate).exists():
            return str(candidate)
    raise SystemExit(f'{name} not found: install LLVM and lld (see README)')


def elf_sections(elf):
    """Allocated, non-empty sections of a linked ELF32 as [(flash address, bytes)], lowest first."""
    data = elf.read_bytes()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    headers = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx]
    out = []
    for name_offset, kind, flags, addr, offset, size, *_ in headers:
        start = names[4] + name_offset
        name = data[start:data.index(b'\0', start)].decode()
        if flags & 0x2 and size and kind != 8:  # SHF_ALLOC, not SHT_NOBITS
            out.append((name, addr, data[offset:offset + size]))
    return sorted(out, key=lambda s: s[1])


def link_added_code(sources, script, includes=(), keep=()):
    """Compile and link C sources at the addresses the linker script gives.

    Returns ([(flash address, bytes)], {symbol: address}). A linker script may place
    data in more than one region, so every allocated section is returned, not just .text."""
    with tempfile.TemporaryDirectory() as tmp:
        objects = []
        for source in sources:
            obj = Path(tmp) / (Path(source).stem + '.o')
            subprocess.run([find_clang(), *ARM_CFLAGS, *(f'-I{i}' for i in includes), '-c', str(source), '-o', str(obj)],
                           check=True)
            objects.append(str(obj))
        elf = Path(tmp) / 'added.elf'
        subprocess.run([find_tool('ld.lld'), '-T', str(script), '--gc-sections', '-e', keep[0],
                        *(f'--undefined={name}' for name in keep[1:]), '-o', str(elf), *objects], check=True)
        sections = [(addr, blob) for _, addr, blob in elf_sections(elf)]
        listing = subprocess.run([find_tool('llvm-nm'), str(elf)], check=True, capture_output=True, text=True).stdout
        symbols = {}
        for line in listing.splitlines():
            parts = line.split()
            if len(parts) == 3:
                symbols[parts[2]] = int(parts[0], 16)
        return sections, symbols


# BLs to the Cust_Info writer 0x080104B8, as file offsets. 0x6E92 is the power-off path, which
# causes nearly all of the wear. 0x737E is left alone: it is the restore-defaults path, which
# always changes the record anyway, and it sits inside the protected flash-driver range.
CUST_INFO_STORE_CALLS = (0x6E92, 0xBE9C, 0xBF2A, 0xBF4E, 0xBF6E, 0xEF00, 0x13BAE)

# Vendor bytes the patches strand, confirmed unreachable by scripts/check_dead_code.py.
# Read-only data may be placed here; keep this in step with that script's DEAD_RANGES.
DEAD_RANGES = ((0x0800F648, 0x0800F808), (0x080180C0, 0x080181AA), (0x08018DB4, 0x08018F0C),
               (0x080708F8, 0x08071E58), (0x08026B34, 0x0802A8FC), (0x0802C4AC, 0x0802EAD0), (0x0802FA09, 0x08031EBD),
               (0x080323D1, 0x08034359), (0x080584F9, 0x0806E2A7))

# Pictures nothing draws once the grid menu, the QC test and the two Chinese pictures are gone:
# the grid icons 0-9, the warning title 21, the company name 22 and the QC test picture 23. Their
# entries in the getter's tbb table (0x0800E8EA, one byte per index) branch to "movs r0, #0"
# instead, so a path that still asks draws nothing (the drawer 0x0800F422 returns for NULL).
FREED_PICTURES = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 21, 22, 23)
PICTURE_GETTER_TABLE = 0x58EA
PICTURE_NULL = 0x28  # (0x0800E93A - 0x0800E8EA) / 2

# Chinese is removed so its glyph table can hold added data (docs/lpm-10c-bootloader-analysis.md,
# "Chinese glyphs"). The blitter 0x08018DB0 is the only code that forms an address in the table.
CJK_BLITTER = 0xFDB0
LANGUAGE_LOAD_CALLS = (0x6E78, 0x6E82)  # boot: Cust_Info load 0x08010288, retried while it returns 0
LANGUAGE_DEFAULTS_CALL = 0x76F4  # factory reset: defaults 0x0801B340, which choose Chinese
LANGUAGE_TO_CHINESE = (0x842A, 0x87D2)  # "movs r0, #2" before the language store, in the two toggles

# Link status replaces the vendor QC test (menu index 4, system state 8).
# Function -> (BL file offset, vendor target replaced).
LINK_STATUS_CALLS = {
    'lpm_link_open': (0x4830, 0x800BAE4),   # key event 6 (M on home): QC open
    'lpm_link_run': (0x48CC, 0x800BD98),    # key event 0x0B: QC count init, only reached from state 8
    'lpm_link_draw': (0x6ABE, 0x800BB0C),   # GUI message 0x0C: QC draw
    'lpm_tool_frame': (0x6C20, 0x800EE8C),  # GUI message 0x36: tool frame; passes other states through
}
# Ping monitor replaces the vendor PING tool (menu index 7, system state 11); ping_monitor.c says why each.
PING_CALLS = {
    'lpm_ping_open': (0x484E, 0x8013794),   # key event 6 (M on home): PING open
    'lpm_ping_back': (0x4890, 0x80137B8),   # key event 7 (BACK) in state 11
    'lpm_ping_key': (0x4984, 0x8014778),    # key events 0x25-0x2A (states 11 and 12); passes IP SCAN through
    'lpm_ping_draw': (0x6B04, 0x801378E),   # GUI message 0x15: bx lr in the vendor
    'lpm_ping_tick_work': (0x1A3E6, 0x8015114),  # 1 ms tick, states 11 and 12: the vendor PING/SCAN network work
}
LINK_STATUS_DATA = [
    # Key table 0x08026264, entry 19 {state mask 0x100, key, press, event 0x0B}: right key held -> M pressed.
    (0x1D300, b'\x01\x08', b'\x04\x03', 'key table entry 19 (state 8, event 0x0B): key 1 held -> key 4 (M) pressed'),
    # Entries 2 and 3, UP and DOWN on the home screen (state mask 0x004, events 2 and 3, which reach
    # lpm_home_key): also in state 8, where lpm_home_key passes them to lpm_link_key to scroll.
    (0x1D274, b'\x04\x00\x00\x00', b'\x04\x01\x00\x00', 'key table entry 2 (UP, event 2): states 2 -> 2 and 8'),
    (0x1D27C, b'\x04\x00\x00\x00', b'\x04\x01\x00\x00', 'key table entry 3 (DOWN, event 3): states 2 -> 2 and 8'),
    # LEFT and RIGHT, short presses only, change the Link status tab (Checklist, Switch port).
    (0x1D284, b'\x04\x00\x00\x00', b'\x04\x01\x00\x00', 'key table entry 4 (LEFT, event 4): states 2 -> 2 and 8'),
    (0x1D28C, b'\x04\x00\x00\x00', b'\x04\x01\x00\x00', 'key table entry 5 (RIGHT, event 5): states 2 -> 2 and 8'),
    # The 1 s tick 0x080232C8 sends the QC count task a measure command while in state 8.
    (0x1A30C, b'\x04\xd1', b'\x04\xe0', 'tick 0x0802330C: bne -> b, no QC count command in state 8'),
]

# The theme (theme.c): the entries of the vendor drawing routines branch to replacements that map
# vendor colours to the theme and draw Terminus. Flash address of the vendor routine -> function.
THEME_ROUTINES = {
    'lpm_theme_clear': 0x080182A4,      # clear(colour)
    'lpm_theme_point': 0x08018450,      # draw_point(x, y), colour at 0x200001A0; pictures draw through it
    'lpm_theme_fill_rect': 0x08018568,  # fill_rect(x0, y0, x1, y1, colour)
    'lpm_theme_plot': 0x080188A4,       # plot(x, y, colour), 40 rows lower
    'lpm_theme_char': 0x08018A34,       # draw_char(x, y, c, size, mode)
}

TOOL_TITLE_CALL = 0x63F8  # 0x0800F3F8: draw_string(13, 21, 88, 20, 16, menu label) in the tool title

# Chinese pictures in vendor screens: the reset warning title (picture 0x15, through 0x0800F412)
# and the About company name (picture 0x16, 0x0800F422; the second call is the Chinese branch).
RESET_WARNING_PICTURE = 0x8D3C
COMPANY_NAME_PICTURE = (0x8B0C, 0x8BCE)


HOME_UI_SOURCES = ('home_ui.c', 'link_status.c', 'switch_port.c', 'ping_monitor.c', 'draw.c', 'cust_info.c',
                   'vendor_text.c', 'subset_font.c', 'theme.c')
HOME_UI_KEEP = ('lpm_home_draw', 'lpm_home_key', 'lpm_cust_info_store_if_changed', 'lpm_cust_info_load_english',
                'lpm_cust_info_defaults_english', 'lpm_reset_warning_title', *LINK_STATUS_CALLS, *PING_CALLS,
                *THEME_ROUTINES, 'lpm_tool_title')


def home_ui_sources():
    return [ROOT / 'src' / 'firmware_patch' / name for name in HOME_UI_SOURCES] + [ROOT / 'src' / 'lpm_ui' / 'lpm_assets.c']


def home_ui():
    """Home screen in the working design, drawn by src/lpm_ui; linked by lld after the vendor payload."""
    ui = ROOT / 'src' / 'lpm_ui'
    patch_dir = ROOT / 'src' / 'firmware_patch'
    sections, symbols = link_added_code(home_ui_sources(), patch_dir / 'home_ui.ld', includes=[ui, patch_dir],
                                        keep=HOME_UI_KEEP)
    if symbols['lpm_home_draw'] != PAYLOAD_END + BASE:
        raise ValueError('home_ui.ld must place lpm_home_draw at the old payload end 0x0807244C')
    draw, key = symbols['lpm_home_draw'] - BASE, (symbols['lpm_home_key'] & ~1) - BASE
    store = (symbols['lpm_cust_info_store_if_changed'] & ~1) - BASE
    load = (symbols['lpm_cust_info_load_english'] & ~1) - BASE
    defaults = (symbols['lpm_cust_info_defaults_english'] & ~1) - BASE
    link = {name: (symbols[name] & ~1) - BASE for name in LINK_STATUS_CALLS}
    main = [(a, b) for a, b in sections if a >= PAYLOAD_END + BASE]
    reused = [(a, b) for a, b in sections if a < PAYLOAD_END + BASE]
    code = main[0][1] if main else b''
    code += bytes(-len(code) % 4)
    if PAYLOAD_END + len(code) > FLASH_LIMIT:
        raise ValueError(f'home_ui is {len(code)} bytes; at most {FLASH_LIMIT - PAYLOAD_END} fit in the update file')
    # Sections the linker script put in the stranded vendor routines must land inside a checked range.
    for address, blob in reused:
        if not any(low <= address and address + len(blob) <= high for low, high in DEAD_RANGES):
            raise ValueError(f'linked section at {address:#x} is outside the ranges cleared for reuse')
    patches = [
        (0x68FD8, b'Length\x00', b'LENGTH\x00', 'main-menu label (also used in the UART log)'),
        redirect_call(0x6A7E, 0x800F648, draw, 'menu entry 0x0800FA7E: grid draw -> lpm_home_draw'),
        redirect_call(0x6A9E, 0x800F648, draw, 'menu redraw 0x0800FA9E: grid draw -> lpm_home_draw'),
        redirect_call(0x4802, 0x80180C0, key, 'menu keys 0x0800D802: grid navigation -> lpm_home_key'),
    ]
    # Every call to the Cust_Info writer 0x080104B8 goes through the unchanged-check first.
    patches += [redirect_call(site, 0x80104B8, store, f'Cust_Info store {site + BASE:#010x} -> skip if unchanged')
                for site in CUST_INFO_STORE_CALLS]
    # English only, so nothing needs the CJK glyph table.
    patches.append((CJK_BLITTER, bytes.fromhex('2de9ff4f'), bytes.fromhex('704700bf'),
                    'CJK glyph blitter 0x08018DB0: push.w -> bx lr; nop'))
    patches += [redirect_call(site, 0x8010288, load, f'Cust_Info load {site + BASE:#010x} -> then pin English')
                for site in LANGUAGE_LOAD_CALLS]
    patches.append(redirect_call(LANGUAGE_DEFAULTS_CALL, 0x801B340, defaults,
                                 'factory reset 0x080106F4: Cust_Info defaults -> then pin English'))
    patches += [(site, b'\x02\x20', b'\x01\x20', f'language toggle {site + BASE:#010x}: movs r0, #2 -> #1 (English)')
                for site in LANGUAGE_TO_CHINESE]
    # Link status in the QC test slot (link_status.c lists what each one replaces).
    patches += [redirect_call(site, old, link[name], f'{site + BASE:#010x}: {old:#010x} -> {name}')
                for name, (site, old) in LINK_STATUS_CALLS.items()]
    patches += LINK_STATUS_DATA
    patches += [redirect_call(site, old, (symbols[name] & ~1) - BASE, f'{site + BASE:#010x}: {old:#010x} -> {name}')
                for name, (site, old) in PING_CALLS.items()]
    patches.append(redirect_call(TOOL_TITLE_CALL, 0x8018D48, (symbols['lpm_tool_title'] & ~1) - BASE,
                                 'tool frame title 0x0800F3F8: draw_string -> lpm_tool_title (home list names)'))
    vendor = VENDOR.read_bytes()
    patches += [(entry - BASE, vendor[entry - BASE:entry - BASE + 4],
                 thumb_branch(entry - BASE, (symbols[name] & ~1) - BASE, link=False),
                 f'theme: {entry:#010x} -> b.w {name}') for name, entry in THEME_ROUTINES.items()]
    # The two Chinese pictures left in English screens (vendor_text.c).
    title = (symbols['lpm_reset_warning_title'] & ~1) - BASE
    patches.append(redirect_call(RESET_WARNING_PICTURE, 0x800F412, title,
                                 'reset warning 0x08011D3C: picture 0x15 -> lpm_reset_warning_title'))
    patches += [(site, VENDOR.read_bytes()[site:site + 4], b'\x00\xbf\x00\xbf',
                 f'About {site + BASE:#010x}: picture 0x16 (company name) -> nop; nop') for site in COMPANY_NAME_PICTURE]
    table = VENDOR.read_bytes()[PICTURE_GETTER_TABLE:PICTURE_GETTER_TABLE + 24]
    freed = bytes(PICTURE_NULL if index in FREED_PICTURES else b for index, b in enumerate(table))
    patches.append((PICTURE_GETTER_TABLE, table, freed, 'picture getter 0x0800E8E0: pictures 0-9 and 21-23 -> NULL'))
    patches.append((PAYLOAD_END, bytes(len(code)), code, f'home_ui, link_status and cust_info code, {len(code)} bytes'))
    patches += [(address - BASE, VENDOR.read_bytes()[address - BASE:address - BASE + len(blob)], blob,
                 f'reused stranded vendor bytes at {address:#010x}, {len(blob)} bytes')
                for address, blob in reused]
    return patches


# name -> list of (file offset, vendor bytes, new bytes, note), or a function that returns one
PATCHES = {
    'label-length-uppercase': [
        (0x68FD8, b'Length\x00', b'LENGTH\x00', 'main-menu label, page 1 slot 3 (180,190)'),
    ],
    # First code change. The Auto Off tick (0x0800FEAC) adds 1 to a counter and powers off once it
    # passes the limit for the setting, read from the halfword table at 0x0806F2E4: 0, 300, 600, 900.
    # Adding 5 per tick makes each choice 5x shorter; the setting labels are changed to match.
    'auto-off-5x': [
        (0x68FD8, b'Length\x00', b'LENGTH\x00', 'main-menu label, page 1 slot 3 (180,190)'),
        (0x6EC6, b'\x40\x1c', b'\x40\x1d', 'Auto Off tick 0x0800FEC6: adds r0, r0, #1 -> adds r0, r0, #5'),
        (0x7B58, b'5min\x00', b'1min\x00', 'Auto Off option 1 label'),
        (0x7B60, b'10min\x00', b'2min\x00\x00', 'Auto Off option 2 label'),
        (0x7B68, b'15min\x00', b'3min\x00\x00', 'Auto Off option 3 label'),
    ],
    # Boundary test: extends the payload past the vendor end and runs the added code at boot.
    'extended-hook': extension_hook(),
    # The main menu as one scrolling list instead of three 2x2 pages; needs clang.
    'menu-list': menu_list,
    # The home screen of output/screens/01-home.png; needs clang and lld. Grows the update file.
    'home-ui': home_ui,
}


def patch_list(name):
    patches = PATCHES[name]
    return patches() if callable(patches) else patches


def apply_patches(data, patches):
    """Return the patched image, or raise ValueError if any patch breaks a rule."""
    out = bytearray(data)
    payload_end = PAYLOAD_END
    for offset, old, new, note in patches:
        end = offset + len(old)
        if PAYLOAD_END <= offset and len(out) < end <= FLASH_LIMIT:
            out.extend(bytes(end - len(out)))  # added code past the vendor file end lands on new zero padding
        if len(new) != len(old):
            raise ValueError(f'{offset:#x} ({note}): length changes from {len(old)} to {len(new)}')
        for start, stop, reason in PROTECTED:
            if offset < stop and start < end:
                raise ValueError(f'{offset:#x} ({note}): overlaps {reason}')
        extends = PAYLOAD_END <= offset and end <= FLASH_LIMIT
        if not (PATCHABLE[0] <= offset and end <= PATCHABLE[1]) and not extends:
            raise ValueError(f'{offset:#x} ({note}): outside the application code and read-only data')
        if bytes(out[offset:end]) != old:
            raise ValueError(f'{offset:#x} ({note}): vendor bytes differ, expected {old!r}')
        out[offset:end] = new
        if extends:
            payload_end = max(payload_end, end + -end % 4)
    if payload_end != PAYLOAD_END:
        # The header holds the payload offset, length and last offset (offset + length - 1).
        offset = struct.unpack_from('<I', out, 0x20)[0]
        struct.pack_into('<II', out, 0x24, payload_end - offset, payload_end - 1)
        file_end = max(payload_end + -payload_end % FILE_ALIGN, len(data))
        out = out[:file_end] + bytes(file_end - len(out))
    return bytes(out)


def check_container(data, vendor=None):
    """The header must keep the vendor layout; only the length may grow, in whole words, and the file
    must end with zero padding up to the next 4 KB like the vendor file."""
    offset, length, last = struct.unpack_from('<III', data, 0x20)
    end = offset + length
    if (offset != 0x1000 or length < 0x6844C or length % 4 or last != end - 1 or end > FLASH_LIMIT
            or len(data) != end + -end % FILE_ALIGN or any(data[end:])):
        raise ValueError('update header or padding no longer matches the vendor layout')
    if vendor is not None and data[:0x24] + data[0x2C:0x1000] != vendor[:0x24] + vendor[0x2C:0x1000]:
        raise ValueError('update header changed outside the length fields')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('name', nargs='?', default='label-length-uppercase', choices=sorted(PATCHES))
    parser.add_argument('--out', type=Path, default=ROOT / 'firmware' / 'patched')
    args = parser.parse_args()

    vendor = VENDOR.read_bytes()
    if hashlib.sha256(vendor).hexdigest() != VENDOR_SHA256:
        raise SystemExit(f'{VENDOR} is not the inspected vendor V2.0.7 image')
    patches = patch_list(args.name)
    patched = apply_patches(vendor, patches)
    check_container(patched, vendor)
    changed = [i for i in range(len(vendor)) if vendor[i] != patched[i]]

    folder = args.out / args.name
    folder.mkdir(parents=True, exist_ok=True)
    image = folder / VENDOR.name
    image.write_bytes(patched)
    manifest = {
        'base': {'file': str(VENDOR.relative_to(ROOT)), 'sha256': VENDOR_SHA256},
        'output': {'file': str(image.relative_to(ROOT)), 'sha256': hashlib.sha256(patched).hexdigest(),
                   'bytes': len(patched)},
        'changed_bytes': len(changed),
        'payload_length': f'{struct.unpack_from("<I", patched, 0x24)[0]:#x}',
        'patches': [{'file_offset': f'{o:#x}', 'flash_address': f'{o + BASE:#010x}',
                     'vendor': old.hex(), 'new': new.hex(), 'note': note}
                    for o, old, new, note in patches],
    }
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Wrote {image.relative_to(ROOT)}: {len(changed)} byte(s) changed, '
          f'SHA-256 {manifest["output"]["sha256"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

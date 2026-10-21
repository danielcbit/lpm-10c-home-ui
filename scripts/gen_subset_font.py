#!/usr/bin/env python3
"""Generate a font holding only the characters the patched screens actually draw.

The update file cannot grow past the vendor size, so whole fonts do not fit
beside the rest of the added code. The screens only ever show a few dozen
characters, so this emits those glyphs plus a small lookup table, and leaves the
glyph shapes untouched.

The character set is read from the string literals in the sources that draw with
this font (SCREEN_SOURCES), so adding text to a screen and rebuilding keeps the
font in step. Any character with no glyph is drawn as a blank of the same width,
never as a wrong glyph.

  python3 scripts/gen_subset_font.py        # writes src/firmware_patch/subset_font.c and .h
  python3 scripts/gen_subset_font.py --small design --body design

The default is Terminus 6x12 and 8x16 (src/fonts/terminus-4.49.1, SIL Open Font
License 1.1), chosen on the tester's 240x320 screen over the design fonts, which
render unevenly there (output/font-compare/). Each of the two fonts (small, body)
can come from one of:
  a .bdf    any bitmap font of at most 8 pixels wide
  design    lpm_font_small (5x9) or lpm_font_body (7x11) from src/lpm_ui/lpm_assets.c
  vendor    the vendor firmware's own 6x12 or 8x16 ASCII font, read from the V2.0.7 image
Whatever its cell height, a font is drawn with its baseline where the design
font's is (LPM_SUB_*_DY), so the screen layouts need no change. Bits are one byte
per row, most significant bit left.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / 'src' / 'lpm_ui' / 'lpm_assets.c'
PATCH_DIR = ROOT / 'src' / 'firmware_patch'
FIRST, COUNT = 32, 95  # the design fonts cover printable ASCII
# Always present: digits and the separators that screens build character by character at run time
# (a counter such as "7/10", an IP address, a percentage) never appear as string literals.
BASELINE = set('0123456789./:%-')

TERMINUS = ROOT / 'src' / 'fonts' / 'terminus-4.49.1'
DEFAULT = {'small': TERMINUS / 'ter-u12n.bdf', 'body': TERMINUS / 'ter-u16n.bdf'}
# Rows above the baseline in the design fonts; every other font is placed to share it, because the
# screen layouts were drawn for the design fonts.
DESIGN = {  # role -> (name in lpm_assets.c, rows per glyph, width, advance, line height, baseline row)
    'small': ('lpm_font_small_bits', 9, 5, 6, 12, 7),
    'body': ('lpm_font_body_bits', 11, 7, 8, 16, 9),
}
VENDOR = ROOT / 'firmware' / 'extracted' / 'LPM-10C' / 'LPM-10C_V2.0.7' / 'LPM-10C_TX_V2.0.7_260907.bin'
VENDOR_FONTS = {  # role -> (flash address, width, rows, baseline row); two bytes per column, top bit first
    'small': (0x0806FE94, 6, 12, 10),
    'body': (0x08070308, 8, 16, 14),
}
# The sources whose text() calls draw with this font. Others (menu_list.c, vendor_text.c) draw
# with the vendor routines, so their strings need no glyphs here.
SCREEN_SOURCES = ('home_ui.c', 'link_status.c', 'draw.c')


def read_font(source, name):
    """Return a list of COUNT glyphs, each a list of row bytes."""
    match = re.search(rf'{name}\[\]\s*=\s*{{(.*?)\n}};', source, re.S)
    if not match:
        raise SystemExit(f'{name} not found in {ASSETS}')
    body = re.sub(r'/\*.*?\*/', '', match.group(1), flags=re.S)
    values = [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+', body)]
    rows = next(spec[1] for spec in DESIGN.values() if spec[0] == name)
    if len(values) != COUNT * rows:
        raise SystemExit(f'{name}: expected {COUNT * rows} bytes, found {len(values)}')
    return [values[i * rows:(i + 1) * rows] for i in range(COUNT)]


def design_font(role):
    name, rows, width, advance, line, baseline = DESIGN[role]
    return dict(glyphs=read_font(ASSETS.read_text(), name), rows=rows, width=width, advance=advance, line=line,
                baseline=baseline, source=f'design {name}')


def vendor_font(role):
    address, width, rows, baseline = VENDOR_FONTS[role]
    image = VENDOR.read_bytes()
    size = width * 2
    glyphs = []
    for i in range(COUNT):
        cell = image[address - 0x08009000 + i * size:address - 0x08009000 + (i + 1) * size]
        glyphs.append([sum(0x80 >> c for c in range(width) if cell[2 * c + row // 8] & (0x80 >> row % 8))
                       for row in range(rows)])
    return dict(glyphs=glyphs, rows=rows, width=width, advance=width, line=rows, baseline=baseline,
                source=f'vendor V2.0.7 {width}x{rows} at {address:#010x}')


def bdf_font(path):
    """A BDF bitmap font: each glyph placed in its FONT_ASCENT + FONT_DESCENT cell."""
    text = Path(path).read_text(errors='replace')
    ascent = int(re.search(r'^FONT_ASCENT (\d+)', text, re.M).group(1))
    descent = int(re.search(r'^FONT_DESCENT (\d+)', text, re.M).group(1))
    width = int(re.search(r'^FONTBOUNDINGBOX (\d+)', text, re.M).group(1))
    if width > 8:
        raise SystemExit(f'{path}: {width} pixels wide; at most 8 fit a byte per row')
    rows = ascent + descent
    glyphs = [[0] * rows for _ in range(COUNT)]
    for block in re.findall(r'^STARTCHAR.*?^ENDCHAR', text, re.M | re.S):
        code = int(re.search(r'^ENCODING (-?\d+)', block, re.M).group(1))
        if not FIRST <= code < FIRST + COUNT:
            continue
        w, h, x, y = (int(v) for v in re.search(r'^BBX (-?\d+) (-?\d+) (-?\d+) (-?\d+)', block, re.M).groups())
        lines = block.split('BITMAP', 1)[1].split()[:h]
        top = ascent - (y + h)
        for i, line in enumerate(lines):
            bits = int(line, 16) << (8 - len(line) * 4) if len(line) <= 2 else int(line[:2], 16)
            if 0 <= top + i < rows:
                glyphs[code - FIRST][top + i] = (bits >> x) & 0xFF
    return dict(glyphs=glyphs, rows=rows, width=width, advance=width, line=rows, baseline=ascent,
                source=f'{Path(path).parent.name}/{Path(path).name}')


def load_font(role, spec):
    if spec == 'design':
        return design_font(role)
    if spec == 'vendor':
        return vendor_font(role)
    return bdf_font(spec)


def characters_used(paths):
    """Printable characters appearing in C string literals in the given sources."""
    used = set()
    for path in paths:
        text = path.read_text()
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
        text = re.sub(r'//[^\n]*', '', text)
        text = re.sub(r'(?m)^\s*#\s*include[^\n]*', '', text)  # a header name is not screen text
        for literal in re.findall(r'"((?:[^"\\\n]|\\.)*)"', text):
            literal = literal.replace('\\n', '').replace('\\r', '').replace('\\t', '')
            literal = literal.replace('\\"', '"').replace('\\\\', '\\')
            used |= {c for c in literal if FIRST <= ord(c) < FIRST + COUNT}
    return used


def emit(characters, fonts):
    """Build the .h and .c text for the chosen characters; fonts maps 'small' and 'body' to a loaded font."""
    ordered = sorted(characters)
    slots = {c: i for i, c in enumerate(ordered)}
    index = [slots.get(chr(FIRST + i), 0xFF) for i in range(COUNT)]
    sources = '; '.join(f'{role} {font["source"]}' for role, font in fonts.items())

    header = f'''/* Generated by scripts/gen_subset_font.py -- do not edit.
 *
 * Fonts reduced to the {len(ordered)} characters the patched screens draw, because whole
 * fonts do not fit in what is left of the update file. Characters outside the set
 * map to LPM_SUB_BLANK and are drawn as a space of the same width.
 * Sources: {sources}
 * Glyphs: Terminus Font, Copyright (C) 2020 Dimitar Toshkov Zhekov, under the
 * SIL Open Font License 1.1 (src/fonts/terminus-4.49.1/OFL.TXT).
 */
#ifndef LPM_SUBSET_FONT_H
#define LPM_SUBSET_FONT_H

#include <stdint.h>

#define LPM_SUB_FIRST {FIRST}
#define LPM_SUB_COUNT {COUNT}
#define LPM_SUB_GLYPHS {len(ordered)}
#define LPM_SUB_BLANK 0xFF

'''
    for role, font in fonts.items():
        upper = role.upper()
        dy = DESIGN[role][5] - font['baseline']
        header += (f'#define LPM_SUB_{upper}_W {font["width"]}\n'
                   f'#define LPM_SUB_{upper}_H {font["rows"]}\n'
                   f'#define LPM_SUB_{upper}_ADVANCE {font["advance"]}\n'
                   f'#define LPM_SUB_{upper}_LINE {font["line"]}\n'
                   f'#define LPM_SUB_{upper}_DY ({dy}) /* cell top relative to the y a screen asks for */\n'
                   f'extern const uint8_t lpm_sub_{role}[LPM_SUB_GLYPHS][{font["rows"]}];\n\n')
    header += '''/* ASCII minus LPM_SUB_FIRST -> glyph slot, or LPM_SUB_BLANK. */
extern const uint8_t lpm_sub_index[LPM_SUB_COUNT];

#endif
'''

    printable = ''.join(c for c in ordered).replace('*/', '* /')
    body = f'''/* Generated by scripts/gen_subset_font.py -- do not edit.
 * Characters: {printable!r}
 * Glyphs from Terminus Font, Copyright (C) 2020 Dimitar Toshkov Zhekov,
 * SIL Open Font License 1.1 (src/fonts/terminus-4.49.1/OFL.TXT).
 */
#include "subset_font.h"

'''
    for role, font in fonts.items():
        body += f'const uint8_t lpm_sub_{role}[LPM_SUB_GLYPHS][{font["rows"]}] = {{\n'
        for character in ordered:
            data = font['glyphs'][ord(character) - FIRST]
            label = {'\\': 'backslash', '*': 'asterisk'}.get(character, character)
            body += '    {' + ', '.join(f'0x{v:02x}' for v in data) + f'}},  /* {label} */\n'
        body += '};\n\n'
    body += 'const uint8_t lpm_sub_index[LPM_SUB_COUNT] = {\n'
    for start in range(0, COUNT, 12):
        body += '    ' + ' '.join(f'0x{v:02x},' for v in index[start:start + 12]) + '\n'
    body += '};\n'
    return header, body


def screen_characters():
    return characters_used([PATCH_DIR / name for name in SCREEN_SOURCES]) | BASELINE | {' '}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=PATCH_DIR)
    parser.add_argument('--small', default=str(DEFAULT['small']), help='a .bdf file, design or vendor (default Terminus 6x12)')
    parser.add_argument('--body', default=str(DEFAULT['body']), help='a .bdf file, design or vendor (default Terminus 8x16)')
    args = parser.parse_args()

    fonts = {'small': load_font('small', args.small), 'body': load_font('body', args.body)}
    characters = screen_characters()
    header, body = emit(characters, fonts)
    (args.out / 'subset_font.h').write_text(header)
    (args.out / 'subset_font.c').write_text(body)

    per_glyph = sum(font['rows'] for font in fonts.values())
    total = len(characters) * per_glyph + COUNT
    print(f'{len(characters)} characters from {len(SCREEN_SOURCES)} source files: '
          f'{len(characters)} x {per_glyph} bytes + {COUNT}-byte index = {total} bytes')
    print(f'(the full fonts would be {COUNT * per_glyph} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

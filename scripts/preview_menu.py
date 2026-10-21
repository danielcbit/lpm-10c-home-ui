#!/usr/bin/env python3
"""Render the patched main menu offline by running the firmware code in an emulator.

Loads a built update image and an SRAM snapshot (by default the sanitized
tests/fixtures/sram-sanitized.bin; a dump from scripts/swd_sram_dump.py also
works), then runs the added lpm_menu_draw on a Cortex-M4 emulator. All vendor
drawing code runs unmodified (fill_rect, draw_point, the cursor and write-start
commands, the font routines). Only the LCD controller is
modelled, on the bus: commands to 0x60000000 (0x2A column, 0x2B row, 0x2C memory
write, as configured at 0x20002FCC) and data to 0x60020000. post_message is
recorded, not run. The vendor header tab (0x0801B0A8) is drawn first for context.

A call that does not return within 20 million instructions is reported as a hang,
an invalid memory access or instruction as a fault, and the deepest stack use is
printed, since the tester runs this code on a task stack of unknown size.

Each screen after the first is drawn the way the tester does it: a key press
(the redirected navigation call), then the partial redraw. It must match a full
redraw of the same selection pixel for pixel.

  .venv/bin/python scripts/preview_menu.py                       # menu-list -> output/menu-list/
  .venv/bin/python scripts/preview_menu.py --image firmware/patched/home-ui/LPM-10C_TX_V2.0.7_260907.bin \
      --out output/home-ui

Needs unicorn in .venv (see README). Nothing is sent to the tester.
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x08009000
PAYLOAD_END = 0x6944C
SRAM_BASE, SRAM_BYTES = 0x20000000, 0x24000
MENU_STATE = 0x20000151
TEXT_FG = 0x200001A0
POST_MESSAGE, HEADER_TAB = 0x0800E960, 0x0801B0A8
LCD_CMD, LCD_DATA, LCD_CONFIG = 0x60000000, 0x60020000, 0x20002FCC
MAX_INSTRUCTIONS = 20_000_000
STOP = 0x0807FF00  # return address that ends a call; erased flash, never executed
W, H = 240, 320
ITEMS = 10


def png(path, pixels, width, height):
    """Write RGB565 pixels as an RGB PNG without third-party modules."""
    rows = b''.join(b'\0' + b''.join(bytes(((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31))
                                     for p in pixels[y * width:(y + 1) * width]) for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


class Tester:
    def __init__(self, image, sram):
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
        from unicorn.arm_const import UC_ARM_REG_SP
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(0x08000000, 0x80000)
        self.uc.mem_write(BASE, image)
        self.uc.mem_write(STOP & ~0xFFF, b'\xff' * 0x1000)
        self.uc.mem_map(SRAM_BASE, SRAM_BYTES)
        self.uc.mem_write(SRAM_BASE, sram)
        self.uc.mem_map(0x60000000, 0x100000)  # LCD bus: anything written here is ignored
        self.uc.reg_write(UC_ARM_REG_SP, SRAM_BASE + SRAM_BYTES - 0x100)
        self.fb = [0] * (W * H)
        self.messages = []
        # Command bytes the vendor driver sends: memory write, column address, row address.
        cfg = bytes(sram[LCD_CONFIG - SRAM_BASE:LCD_CONFIG - SRAM_BASE + 10])
        self.cmd_write, self.cmd_col, self.cmd_row = cfg[7], cfg[8], cfg[9]
        self.command, self.params = None, []
        self.col, self.row = [0, W - 1], [0, H - 1]
        self.x = self.y = 0
        self.lowest_sp = SRAM_BASE + SRAM_BYTES - 0x100
        self.code_hook = self.uc.hook_add(UC_HOOK_CODE, self._hook)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._lcd_cmd, begin=LCD_CMD, end=LCD_CMD + 1)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._lcd_data, begin=LCD_DATA, end=LCD_DATA + 1)

    def _lcd_cmd(self, uc, access, address, size, value, _):
        self.command, self.params = value & 0xFF, []
        if self.command == self.cmd_write:
            self.x, self.y = self.col[0], self.row[0]

    def _lcd_data(self, uc, access, address, size, value, _):
        if self.command == self.cmd_write:
            if self.x <= self.col[1] and self.y <= self.row[1] and self.x < W and self.y < H:
                self.fb[self.y * W + self.x] = value & 0xFFFF
            self.x += 1
            if self.x > self.col[1]:
                self.x, self.y = self.col[0], self.y + 1
            return
        self.params.append(value & 0xFF)
        window = self.col if self.command == self.cmd_col else self.row if self.command == self.cmd_row else None
        if window is not None:
            if len(self.params) >= 2:
                window[0] = self.params[0] << 8 | self.params[1]
            if len(self.params) >= 4:
                window[1] = self.params[2] << 8 | self.params[3]

    def _arg(self, n):
        from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP
        if n < 4:
            return self.uc.reg_read(UC_ARM_REG_R0 + n)
        return struct.unpack('<I', self.uc.mem_read(self.uc.reg_read(UC_ARM_REG_SP) + 4 * (n - 4), 4))[0]

    def _return(self):
        from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def _hook(self, uc, address, size, _):
        from unicorn.arm_const import UC_ARM_REG_SP
        sp = uc.reg_read(UC_ARM_REG_SP)
        if sp < self.lowest_sp:
            self.lowest_sp = sp
        if address == POST_MESSAGE:
            self.messages.append(self._arg(0))
            self._return()
        elif address == STOP:
            uc.emu_stop()

    def call(self, address, *args):
        from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0
        for i, value in enumerate(args):
            self.uc.reg_write(UC_ARM_REG_R0 + i, value)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        from unicorn import UcError
        from unicorn.arm_const import UC_ARM_REG_PC
        try:
            self.uc.emu_start(address | 1, STOP, count=MAX_INSTRUCTIONS)
        except UcError as error:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            raise SystemExit(f'FAULT in call to {address:#010x}: {error} at pc {pc:#010x}')
        pc = self.uc.reg_read(UC_ARM_REG_PC)
        if pc & ~1 != STOP:
            raise SystemExit(f'HANG: call to {address:#010x} did not return in {MAX_INSTRUCTIONS} instructions '
                             f'(pc {pc:#010x})')

    def state(self):
        return self.uc.mem_read(SRAM_BASE, SRAM_BYTES)

    def restore(self, sram):
        self.uc.mem_write(SRAM_BASE, bytes(sram))

    def selection(self):
        return self.uc.mem_read(MENU_STATE, 1)[0] - 4

    def select(self, index):
        self.uc.mem_write(MENU_STATE, bytes([4 + index]))


def find_entry(image, vendor_call_site):
    """Target of the BL at a file offset, as a flash address."""
    h1, h2 = struct.unpack_from('<HH', image, vendor_call_site)
    s = (h1 >> 10) & 1
    i1, i2 = 1 - (((h2 >> 13) & 1) ^ s), 1 - (((h2 >> 11) & 1) ^ s)
    offset = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if s:
        offset -= 1 << 25
    return vendor_call_site + BASE + 4 + offset


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path,
                        default=ROOT / 'firmware' / 'patched' / 'menu-list' / 'LPM-10C_TX_V2.0.7_260907.bin')
    parser.add_argument('--sram', type=Path, default=ROOT / 'tests' / 'fixtures' / 'sram-sanitized.bin')
    parser.add_argument('--out', type=Path, default=ROOT / 'output' / 'menu-list')
    args = parser.parse_args()
    try:
        import unicorn  # noqa: F401
    except ImportError:
        raise SystemExit('Needs unicorn: .venv/bin/pip install unicorn (see README)')

    image, sram = args.image.read_bytes(), args.sram.read_bytes()
    draw, nav = find_entry(image, 0x6A7E), find_entry(image, 0x4802)
    if draw < BASE + PAYLOAD_END or nav < BASE + PAYLOAD_END:
        raise SystemExit(f'{args.image} does not redirect the menu to added code')
    labels = [struct.unpack_from('<I', sram, 0x18 + 4 * i)[0] for i in range(ITEMS)]
    names = [image[a - BASE:image.index(b'\0', a - BASE)].decode() for a in labels]
    print('Menu items from SRAM:', ', '.join(names))

    args.out.mkdir(parents=True, exist_ok=True)
    tester = Tester(image, sram)
    tester.call(HEADER_TAB)
    tester.select(0)
    tester.call(draw, 0)
    png(args.out / 'menu-00.png', tester.fb, W, H)

    # Key presses: action 3 (down) then the partial redraw, compared with a full redraw.
    seen, mismatches = [tester.selection()], []
    for step in range(1, ITEMS + 1):
        tester.call(nav, 3)
        tester.call(draw, 1)
        seen.append(tester.selection())
        partial, saved = list(tester.fb), tester.state()
        tester.call(draw, 0)
        if tester.fb != partial:
            mismatches.append(step)
        tester.restore(saved)
        if step < ITEMS:
            png(args.out / f'menu-{step:02d}.png', partial, W, H)
    tester.call(nav, 2)
    up = tester.selection()
    print(f'Selected vendor items while pressing down: {seen}; up then selects {up}')
    used = SRAM_BASE + SRAM_BYTES - 0x100 - tester.lowest_sp
    print(f'Deepest stack use: {used} bytes')
    print(f'Redraw messages posted: {sorted(set(tester.messages))}; partial redraws differing from full: '
          f'{mismatches or "none"}')
    print(f'Wrote {ITEMS} screens to {args.out}/')
    return 0 if not mismatches and set(tester.messages) == {7} and len(set(seen)) == ITEMS else 1


if __name__ == '__main__':
    sys.exit(main())

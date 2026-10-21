#!/usr/bin/env python3
"""Render the vendor tool screens offline by running the firmware in an emulator.

Opens each tool the way the M key on the home screen does (the open routines in
the table at 0x0800D816), then delivers every GUI message the code posts to the
GUI task's own dispatch (0x0800F9D0), as the task would. Each message handler
that the tool can receive is then sent once more on its own, so result screens
are drawn too, from whatever the SRAM snapshot holds (the sanitized fixture has
no measurement, so results show their empty or zero state).

Used to see what the theme changes (src/firmware_patch/theme.c) do to screens
the emulator cannot otherwise reach: the measurement hardware is not modelled,
so these are layouts, not real results.

  .venv/bin/python scripts/preview_vendor.py                     # vendor image -> output/vendor/
  .venv/bin/python scripts/preview_vendor.py --image firmware/patched/home-ui/LPM-10C_TX_V2.0.7_260907.bin \
      --out output/vendor-themed

Peripherals are plain memory, so a register read returns what was last written.
FreeRTOS calls are stubbed. Nothing is sent to the tester.
"""
import argparse
from pathlib import Path
import struct
import sys

import preview_menu as P

ROOT = Path(__file__).resolve().parent.parent
MENU_STATE = 0x20000150
OPEN_TABLE, OPEN_TABLE_BASE = 0x0800D81A, 0x0800D81A
GUI_DISPATCH, GUI_DONE = 0x0800F9D0, 0x0800FC62
GUI_SP = 0x20022000
HEAP, HEAP_END = 0x20022800, 0x20023F00
STOP = P.STOP

# Vendor menu index -> name, for file names (the open table is indexed by menu position).
TOOLS = ['cable-map', 'cable-trace', 'port-flash', 'cable-length', 'link-status', 'link-speed', 'poe',
         'ping', 'ip-scan', 'settings']
# GUI messages with a handler of their own (0x0800F9DC table): drawn once each per tool after opening.
HANDLER_MESSAGES = [0x03, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, *range(0x0F, 0x23), *range(0x23, 0x36), 0x37, 0x3C]


class VendorTester(P.Tester):
    def __init__(self, image, sram):
        super().__init__(image, sram)
        for base, size in ((0x40000000, 0x30000), (0x50000000, 0x10000), (0xA0000000, 0x1000),
                           (0xE0000000, 0x100000)):
            self.uc.mem_map(base, size)
        self.heap = HEAP
        self.queue = []
        self.tick = 0
        self.stubs = {
            0x08023B10: self._malloc,       # pvPortMalloc(size)
            0x08024480: lambda: 0,          # vPortFree
            0x0802443C: lambda: 0,          # critical section enter (msr BASEPRI)
            0x08024460: lambda: 0,          # critical section exit
            0x08024828: lambda: 1,          # xQueueSend
            0x08024940: lambda: 0,          # xQueueReceive: nothing waiting
            0x080244E4: lambda: 0,          # delay(ms)
            0x08023D38: self._tick,         # tick count
        }
        # Only these addresses need Python: hooking every instruction is many times slower.
        from unicorn import UC_HOOK_CODE
        self.uc.hook_del(self.code_hook)
        for address in (*self.stubs, P.POST_MESSAGE, STOP):
            self.uc.hook_add(UC_HOOK_CODE, self._hook, begin=address, end=address)

    def _malloc(self):
        size = (self._arg(0) + 7) & ~7
        if self.heap + size > HEAP_END:
            self.heap = HEAP
        address, self.heap = self.heap, self.heap + size
        return address

    def _tick(self):
        self.tick += 10
        return self.tick

    def _hook(self, uc, address, size, _):
        from unicorn.arm_const import UC_ARM_REG_R0
        if address == P.POST_MESSAGE:
            message, source, length = self._arg(0) & 0xFF, self._arg(1), self._arg(2) & 0xFF
            data = bytes(uc.mem_read(source, length)) if source and length else b''
            self.queue.append((message, data))
            self._return()
            return
        if address in self.stubs:
            uc.reg_write(UC_ARM_REG_R0, self.stubs[address]() & 0xFFFFFFFF)
            self._return()
            return
        super()._hook(uc, address, size, _)

    def gui(self, message, data=b''):
        """One pass of the GUI task's dispatch for a message, as received from its queue."""
        from unicorn import UcError
        from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP
        pointer = 0
        if data:
            self.uc.reg_write(UC_ARM_REG_SP, GUI_SP)
            pointer = self.heap
            self.heap += (len(data) + 7) & ~7
            self.uc.mem_write(pointer, data)
        self.uc.mem_write(GUI_SP + 8, struct.pack('<BBHI', message, len(data), 0, pointer))
        self.uc.reg_write(UC_ARM_REG_SP, GUI_SP)
        try:
            self.uc.emu_start(GUI_DISPATCH | 1, GUI_DONE, count=P.MAX_INSTRUCTIONS)
        except UcError as error:
            return f'message {message:#04x}: {error} at {self.uc.reg_read(UC_ARM_REG_PC):#010x}'
        return None

    def drain(self, limit=400):
        problems = []
        while self.queue and limit:
            message, data = self.queue.pop(0)
            problem = self.gui(message, data)
            if problem:
                problems.append(problem)
            limit -= 1
        return problems

    def run(self, address, *args):
        from unicorn import UcError
        from unicorn.arm_const import UC_ARM_REG_PC
        try:
            self.call(address, *args)
        except (UcError, SystemExit) as error:
            return f'{address:#010x}: {error} at {self.uc.reg_read(UC_ARM_REG_PC):#010x}'
        return None


def open_targets(image):
    """Menu index -> open routine, from the tbb table at 0x0800D816: the first BL at or after each case.
    Port flash, Cable length and Link speed share 0x080134C4, which takes the menu state in r0."""
    out = []
    for index in range(10):
        case = OPEN_TABLE_BASE + 2 * image[OPEN_TABLE - P.BASE + index]
        for _ in range(4):
            h1, h2 = struct.unpack_from('<HH', image, case - P.BASE)
            if (h1 & 0xF800) == 0xF000 and (h2 & 0xD000) == 0xD000:
                out.append(P.find_entry(image, case - P.BASE))
                break
            case += 2
        else:
            out.append(None)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, default=ROOT / 'firmware' / 'extracted' / 'LPM-10C' / 'LPM-10C_V2.0.7'
                        / 'LPM-10C_TX_V2.0.7_260907.bin')
    parser.add_argument('--sram', type=Path, default=ROOT / 'tests' / 'fixtures' / 'sram-sanitized.bin')
    parser.add_argument('--out', type=Path, default=ROOT / 'output' / 'vendor')
    parser.add_argument('--only', help='one tool name')
    parser.add_argument('--opens-only', action='store_true', help='only the screen each tool opens with (fast)')
    args = parser.parse_args()
    try:
        import unicorn  # noqa: F401
    except ImportError:
        raise SystemExit('Needs unicorn: .venv/bin/pip install unicorn (see README)')

    image, sram = args.image.read_bytes(), args.sram.read_bytes()
    args.out.mkdir(parents=True, exist_ok=True)
    targets = open_targets(image)
    written = 0
    for index, name in enumerate(TOOLS):
        if args.only and args.only != name:
            continue
        tester = VendorTester(image, sram)
        tester.uc.mem_write(MENU_STATE, bytes([2, 4 + index]))
        problems = []
        if targets[index] is None:
            print(f'{name}: no open routine found')
            continue
        problem = tester.run(targets[index], 4 + index)  # r0: the menu state, for the shared routine
        problems += [problem] if problem else []
        problems += tester.drain()
        P.png(args.out / f'{name}.png', tester.fb, P.W, P.H)
        written += 1
        base_fb, base_ram = list(tester.fb), tester.state()
        seen = {tuple(base_fb)}
        for message in ([] if args.opens_only else HANDLER_MESSAGES):
            tester.restore(base_ram)
            tester.fb = list(base_fb)
            problem = tester.gui(message)
            extra = tester.drain()
            if problem or extra:
                continue
            key = tuple(tester.fb)
            if key in seen:
                continue
            seen.add(key)
            P.png(args.out / f'{name}-{message:02x}.png', tester.fb, P.W, P.H)
            written += 1
        state = tester.uc.mem_read(MENU_STATE, 1)[0]
        print(f'{name:13s} open {targets[index]:#010x}  state {state:2d}  screens {len(seen)}'
              + (f'  problems: {problems[:2]}' if problems else ''))
    print(f'Wrote {written} screens to {args.out}/')
    return 0


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())

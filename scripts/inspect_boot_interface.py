#!/usr/bin/env python3
"""Record the TX application's interface to the bootloader and its flash storage (Capstone 5).

Checks the update container for a checksum, lists every flash address the
application references outside its own code, and disassembles the routines
that write flash. Static analysis of the V2.0.7 file only; nothing touches a device.
"""
from pathlib import Path
import hashlib
import struct
import zlib
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

source = Path('firmware/extracted/LPM-10C/LPM-10C_V2.0.7/LPM-10C_TX_V2.0.7_260907.bin')
data = source.read_bytes()
expected = '3bcc50065e16e93ddeff5fb9dce04f1b0447fd3f68a084803070978aef2bcaaa'
assert hashlib.sha256(data).hexdigest() == expected, 'Offsets apply only to the inspected TX release'
BASE = 0x08009000  # VA = file offset + BASE; the payload at file 0x1000 loads at 0x0800A000

ranges = {
    'main(): VTOR, UID binding check, V2 marker check, both hang on failure': (0x1a204, 0x1a244),
    'UID binding against the info page at 0x08009000': (0x1b63c, 0x1b6a0),
    'Info page writer: erase 0x08009000, program words, write "_V2." at +0x6c': (0x10634, 0x10694),
    'V2 marker check: byte at 0x0800906e must be 0x32': (0xdff4, 0xe004),
    'BootFlag storage: 0x9718 at 0x0807f000': (0x7030, 0x7058),
    'BootFlag check: read 0x0807f000, erase if 0x9718': (0x71b4, 0x71b8),
    'Cust_Info init: 200 bytes from 0x0807f800, valid if +0xa0 == 0x9718': (0x72ee, 0x7384),
    'Cust_Info storage: erase and program 0x0807f800, compare': (0x7524, 0x7564),
    'Flash driver: lock, program word, unlock (keys 0x45670123, 0xcdef89ab)': (0xdf00, 0xdf78),
}


def u32(offset):
    return struct.unpack_from('<I', data, offset)[0]


def stm32_crc(body):
    crc = 0xFFFFFFFF
    for (word,) in struct.iter_unpack('<I', body[:len(body) // 4 * 4]):
        crc ^= word
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


offset, length, last = struct.unpack_from('<III', data, 0x20)
payload = data[offset:offset + length]
lines = [f'Source: {source}', f'SHA-256: {expected}',
         'VA = file offset + 0x08009000. Linear decoding includes literal pools; do not interpret data as code.',
         '', 'CONTAINER',
         f'name {data[:0x20].split(bytes(1))[0].decode()}, payload offset {offset:#x}, length {length:#x}, '
         f'last {last:#x} (offset + length - 1: {last == offset + length - 1})',
         f'header bytes 0x2c-0xfff all zero: {not any(data[0x2c:0x1000])}; '
         f'bytes after payload all zero: {not any(data[offset + length:])}']
for name, value in (('CRC-32', zlib.crc32(payload)), ('STM32 hardware CRC', stm32_crc(payload)),
                    ('byte sum', sum(payload) & 0xFFFFFFFF),
                    ('word sum', sum(w for (w,) in struct.iter_unpack('<I', payload[:length // 4 * 4])) & 0xFFFFFFFF)):
    packed = struct.pack('<I', value), struct.pack('>I', value)
    found = [f'{i:#x}' for i in range(len(data) - 3) if data[i:i + 4] in packed]
    lines.append(f'{name} of payload {value:#010x}: stored in file at {found or "nowhere"}')

lines += ['', 'FLASH ADDRESSES OUTSIDE THE APPLICATION CODE (aligned literals in the payload)']
for i in range(offset, offset + length - 3, 4):
    value = u32(i)
    if 0x08000000 <= value < 0x0800A000 or 0x0807244C < value < 0x08080000 or value == 0x1FFFF7F0:
        lines.append(f'file {i:#07x}  {value:#010x}')
lines.append('0x08000014 is loaded only by floating-point library code at file 0x1c62a. The other values in\n'
             '0x08000000-0x08008400 sit in image data and no pc-relative load reads them.')

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.skipdata = True
for title, (start, end) in ranges.items():
    lines.append(f'\nRANGE file 0x{start:x}–0x{end:x}: {title}')
    for ins in md.disasm(data[start:end], start + BASE):
        lines.append(f'{ins.address - BASE:08x} {ins.address:08x} {ins.mnemonic:8} {ins.op_str}')
Path('evidence/tx-boot-interface-disassembly.txt').write_text('\n'.join(lines) + '\n')
print('Wrote evidence/tx-boot-interface-disassembly.txt')

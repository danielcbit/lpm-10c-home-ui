#!/usr/bin/env python3
"""Record focused Thumb disassembly of observed TX networking routines (Capstone 5)."""
from pathlib import Path
import hashlib
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

source = Path('firmware/extracted/LPM-10C/LPM-10C_V2.0.7/LPM-10C_TX_V2.0.7_260907.bin')
data = source.read_bytes()
expected = '3bcc50065e16e93ddeff5fb9dce04f1b0447fd3f68a084803070978aef2bcaaa'
assert hashlib.sha256(data).hexdigest() == expected, 'Offsets apply only to the inspected TX release'
ranges = [(0x1425c, 0x14700), (0x16d86, 0x1711c), (0x18a60, 0x18d90),
          (0x19408, 0x1977c), (0x19810, 0x198b8), (0x1ad44, 0x1af2c),
          (0x1af64, 0x1b0dc)]
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.skipdata = True
lines = [f'Source: {source}', f'SHA-256: {expected}',
         'VA = file offset + 0x08009000. Linear decoding includes literal pools; do not interpret data as code.']
for start, end in ranges:
    lines.append(f'\nRANGE file 0x{start:x}–0x{end:x}')
    for ins in md.disasm(data[start:end], start + 0x08009000):
        lines.append(f'{ins.address - 0x08009000:08x} {ins.address:08x} {ins.mnemonic:8} {ins.op_str}')
Path('evidence/tx-network-disassembly.txt').write_text('\n'.join(lines) + '\n')
print('Wrote evidence/tx-network-disassembly.txt')

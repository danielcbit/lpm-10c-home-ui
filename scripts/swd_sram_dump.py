#!/usr/bin/env python3
"""Snapshot the tester's SRAM over SWD, read-only, and look for the settings record.

Flash is unreadable at read-protection level 1, but the running firmware keeps
a RAM copy of its 200-byte Cust_Info settings record (flash 0x0807F800). The
record is valid when the half-word at +0xA0 is 0x9718 (docs/lpm-10c-bootloader-analysis.md).
The core is not halted, so the snapshot is not atomic.

Saves evidence/swd/sram-<time>.{bin,log,json}. Unreadable chunks are zero-filled
in the .bin and listed in the .json.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

import swd_identify

ROOT = Path(__file__).resolve().parent.parent
TCL = ROOT / 'scripts' / 'swd' / 'n32g45x_sram_dump.tcl'
SRAM_BASE, SRAM_BYTES = 0x20000000, 0x24000
CUST_INFO_BYTES, CUST_INFO_MAGIC_OFFSET, CUST_INFO_MAGIC = 200, 0xA0, 0x9718
CUST_INFO_RAM_V2_0_7 = 0x20002CF0


def parse_chunks(text):
    """Return ({address: bytes}, {address: error}) from 'LPM sram' lines."""
    chunks, errors = {}, {}
    for line in text.splitlines():
        m = re.match(r'\s*LPM sram (0x[0-9a-fA-F]+) (.*)$', line)
        if not m:
            continue
        address, rest = int(m.group(1), 16), m.group(2)
        if rest.startswith('error'):
            errors[address] = rest[len('error'):].strip()
        else:
            chunks[address] = b''.join(int(w, 16).to_bytes(4, 'little') for w in rest.split(','))
    return chunks, errors


def assemble(chunks):
    image = bytearray(SRAM_BYTES)
    for address, data in chunks.items():
        start = address - SRAM_BASE
        if 0 <= start < SRAM_BYTES:
            image[start:start + len(data)] = data[:SRAM_BYTES - start]
    return bytes(image)


def find_cust_info(image):
    """RAM addresses of candidate Cust_Info copies: magic at +0xA0, record not all zero or 0xFF."""
    magic = CUST_INFO_MAGIC.to_bytes(2, 'little')
    found = []
    for start in range(0, len(image) - CUST_INFO_BYTES + 1, 4):
        record = image[start:start + CUST_INFO_BYTES]
        if record[CUST_INFO_MAGIC_OFFSET:CUST_INFO_MAGIC_OFFSET + 2] != magic:
            continue
        rest = record[:CUST_INFO_MAGIC_OFFSET] + record[CUST_INFO_MAGIC_OFFSET + 2:]
        if rest.count(0) < len(rest) - 4 and rest.count(0xFF) < len(rest) - 4:
            found.append(SRAM_BASE + start)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--probe', choices=sorted(swd_identify.PROBES), default='ft232h')
    parser.add_argument('--speed', type=int, default=1000, help='SWD clock in kHz (default 1000)')
    parser.add_argument('--openocd', default=shutil.which('openocd') or 'openocd')
    parser.add_argument('--out', type=Path, default=ROOT / 'evidence' / 'swd')
    args = parser.parse_args()

    command = [args.openocd, *swd_identify.PROBES[args.probe], '-c', f'adapter speed {args.speed}', '-f', str(TCL)]
    if not shutil.which(args.openocd):
        print('OpenOCD not found: install it with "brew install open-ocd"', file=sys.stderr)
        return 2
    print(f'Reading {SRAM_BYTES // 1024} KB of SRAM at {args.speed} kHz; the tester keeps running.')
    try:
        log = subprocess.run(command, capture_output=True, text=True, timeout=600)
        log = log.stdout + log.stderr
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
        log += '\n[swd_sram_dump] OpenOCD timed out after 600 s\n'

    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    args.out.mkdir(parents=True, exist_ok=True)
    base = args.out / f'sram-{stamp}'
    # The .log keeps only OpenOCD's own messages; the data lines go to the .bin.
    Path(f'{base}.log').write_text(''.join(l + '\n' for l in log.splitlines() if not re.match(r'\s*LPM sram 0x\S+ [0-9a-f]', l)))
    chunks, errors = parse_chunks(log)
    read_bytes = sum(len(c) for a, c in chunks.items() if a != SRAM_BASE or len(c) > 4)
    if read_bytes == 0:
        print(log[-2000:])
        print('No SRAM was readable.' + (' The probe read failed: SRAM is probably blocked at L1 on this part.'
                                         if errors else ' Check wiring and that the tester is on.'), file=sys.stderr)
        Path(f'{base}.json').write_text(json.dumps({'readable_bytes': 0, 'errors': {f'0x{a:08x}': e for a, e in errors.items()}}, indent=2) + '\n')
        return 1

    image = assemble({a: c for a, c in chunks.items() if len(c) > 4})
    Path(f'{base}.bin').write_bytes(image)
    candidates = find_cust_info(image)
    summary = {
        'date': datetime.now().astimezone().isoformat(timespec='seconds'),
        'probe': args.probe,
        'readable_bytes': read_bytes,
        'unreadable_chunks': {f'0x{a:08x}': e for a, e in errors.items()},
        'cust_info_candidates': [f'0x{a:08x}' for a in candidates],
        'cust_info_at_v2_0_7_address': f'0x{CUST_INFO_RAM_V2_0_7:08x}' in [f'0x{a:08x}' for a in candidates],
    }
    for address in candidates:
        start = address - SRAM_BASE
        record = image[start:start + CUST_INFO_BYTES]
        Path(f'{base}-cust_info-{address:08x}.bin').write_bytes(record)
    Path(f'{base}.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'Read {read_bytes} of {SRAM_BYTES} bytes; {len(errors)} unreadable chunk(s).')
    print(f'Cust_Info candidates: {", ".join(summary["cust_info_candidates"]) or "none"}')
    print(f'Saved {base}.bin, .json and .log' + (' plus one -cust_info-*.bin per candidate.' if candidates else '.'))
    return 0


if __name__ == '__main__':
    sys.exit(main())

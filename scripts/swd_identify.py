#!/usr/bin/env python3
"""Identify the tester's MCU and its read-protection level over SWD, read-only.

Runs OpenOCD with scripts/swd/n32g45x_identify.tcl, decodes the registers and
saves the raw log plus a report under evidence/swd/. Nothing is written to the
chip: no flash, option-byte, halt or reset operations.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
TCL = ROOT / 'scripts' / 'swd' / 'n32g45x_identify.tcl'

PROBES = {
    # Clone J-Links work through OpenOCD's libjaylink driver, which never updates probe firmware.
    'jlink': ['-f', 'interface/jlink.cfg', '-c', 'transport select swd'],
    # FT232H: D0 -> SWCLK, D1 -> 470 ohm -> SWDIO, D2 -> SWDIO. The config selects SWD itself.
    'ft232h': ['-f', 'interface/ftdi/ft232h-module-swd.cfg'],
}

DEVICES = {0x452: 'N32G452', 0x455: 'N32G455', 0x457: 'N32G457'}
# Initial stack pointer and reset vector of the vendor V2.0.7 image (docs/lpm-10c-investigation.md).
VENDOR_APP_VECTORS = (0x20011298, 0x0800A355)


def parse_output(text):
    """Map each 'LPM <name> ...' line to a list of words, or to an error string."""
    reads = {}
    for line in text.splitlines():
        m = re.match(r'\s*LPM (\w+) (.*)$', line)
        if not m:
            continue
        name, rest = m.groups()
        if rest.startswith('error'):
            reads[name] = rest[len('error'):].strip() or 'error'
        else:
            reads[name] = [int(word, 16) for word in rest.split(',')]
    m = re.search(r'SWD DPIDR (0x[0-9a-fA-F]+)', text)
    dpidr = int(m.group(1), 16) if m else None
    return reads, dpidr


def decode_dbg_id(value):
    """DBG_ID (manual 29.4.2): device number split into high/middle/low nibbles."""
    device = ((value >> 12) & 0xF) << 8 | ((value >> 8) & 0xF) << 4 | ((value >> 20) & 0xF)
    return {
        'device': f'0x{device:03x}',
        'model': DEVICES.get(device, 'unknown'),
        'sram_kb': (((value >> 28) & 0xF) + 1) * 16,
        'flash_kb': ((value >> 16) & 0xF) * 64,
        'revision': f'0x{value & 0xFF:02x}',
    }


def decode_flash_ob(value):
    """FLASH_OB (manual 3.2.5.2.7). L2 disables debug, so reading it at all implies L0 or L1."""
    rdp1, rdp2 = bool(value & 0x2), bool(value & 0x80000000)
    return {
        'read_protection': 'L2' if rdp2 else 'L1' if rdp1 else 'L0 (unprotected)',
        'option_byte_error': bool(value & 0x1),
        'software_watchdog': bool(value & 0x4),
        'data0': f'0x{(value >> 10) & 0xFF:02x}',
        'data1': f'0x{(value >> 18) & 0xFF:02x}',
    }


def summarize(reads, dpidr):
    summary = {'swd_dpidr': f'0x{dpidr:08x}' if dpidr is not None else None}
    for name, decode in (('dbg_id', decode_dbg_id), ('flash_ob', decode_flash_ob)):
        value = reads.get(name)
        summary[name] = decode(value[0]) if isinstance(value, list) else {'error': value or 'not read'}
    for name in ('uid', 'ucid', 'option_bytes', 'flash_wrp', 'boot_vectors'):
        value = reads.get(name)
        summary[name] = [f'0x{w:08x}' for w in value] if isinstance(value, list) else {'error': value or 'not read'}
    app = reads.get('app_vectors')
    if isinstance(app, list):
        summary['app_vectors'] = {'words': [f'0x{w:08x}' for w in app],
                                  'matches_vendor_v2_0_7': tuple(app) == VENDOR_APP_VECTORS}
    else:
        summary['app_vectors'] = {'error': app or 'not read'}
    return summary


def report_lines(summary):
    lines = []
    dbg, ob, app = summary['dbg_id'], summary['flash_ob'], summary['app_vectors']
    lines.append(f"SWD DPIDR:        {summary['swd_dpidr'] or 'not reported'}")
    if 'error' in dbg:
        lines.append(f"Device:           not read ({dbg['error']})")
    else:
        lines.append(f"Device:           {dbg['model']} ({dbg['device']}), revision {dbg['revision']}")
        lines.append(f"Flash / SRAM:     {dbg['flash_kb']} KB / {dbg['sram_kb']} KB")
    if 'error' in ob:
        lines.append(f"Read protection:  not read ({ob['error']})")
    else:
        lines.append(f"Read protection:  {ob['read_protection']}" + (', OPTION BYTE ERROR' if ob['option_byte_error'] else ''))
    if 'error' in app:
        lines.append('App vectors:      not readable (expected under L1)')
    else:
        verdict = 'match' if app['matches_vendor_v2_0_7'] else 'differ from'
        lines.append(f"App vectors:      {' '.join(app['words'])} ({verdict} vendor V2.0.7)")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--probe', choices=sorted(PROBES), default='jlink')
    parser.add_argument('--speed', type=int, default=1000, help='SWD clock in kHz (default 1000)')
    parser.add_argument('--openocd', default=shutil.which('openocd') or 'openocd')
    parser.add_argument('--out', type=Path, default=ROOT / 'evidence' / 'swd')
    parser.add_argument('--dry-run', action='store_true', help='print the OpenOCD command and exit')
    args = parser.parse_args()

    command = [args.openocd, *PROBES[args.probe], '-c', f'adapter speed {args.speed}', '-f', str(TCL)]
    if args.dry_run:
        print(shlex.join(command))
        return 0
    if not shutil.which(args.openocd):
        print('OpenOCD not found: install it with "brew install open-ocd"', file=sys.stderr)
        return 2
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        log = result.stdout + result.stderr
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
        log += '\n[swd_identify] OpenOCD timed out after 60 s\n'
        result = None

    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / f'identify-{stamp}.log').write_text(log)
    reads, dpidr = parse_output(log)
    if not reads:
        print(log)
        print('No registers were read. Check wiring, probe power and VTref; the board must be powered.\n'
              'If the probe connects but the DAP does not answer, the chip may be at L2 (debug disabled)\n'
              'or the firmware may have repurposed the SWD pins. Do not try "unlock" or "mass erase".',
              file=sys.stderr)
        return 1
    summary = summarize(reads, dpidr)
    summary['probe'] = args.probe
    summary['date'] = datetime.now().astimezone().isoformat(timespec='seconds')
    (args.out / f'identify-{stamp}.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('\n'.join(report_lines(summary)))
    print(f'Saved {args.out / f"identify-{stamp}.json"} and the raw OpenOCD log.')
    return 0 if result is not None and result.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

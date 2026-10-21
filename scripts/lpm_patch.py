#!/usr/bin/env python3
"""Make or apply a release patch for the FNIRSI LPM-10C TX firmware V2.0.7.

Releases do not include FNIRSI's firmware. They include a patch: the byte
ranges in which a build differs from the vendor V2.0.7 update file, with the
SHA-256 of the vendor file it applies to and of the file it produces. Applying
it to your own copy of the official update rebuilds the exact image that was
released, or fails without writing anything.

  # Apply: give the official update (the .zip from FNIRSI, or the .bin inside it)
  python3 lpm_patch.py apply LPM-10C.zip lpm-10c-home-ui-v1.0.0.patch.json
  # -> LPM-10C_TX_V2.0.7_260907.bin in the current directory (the name the
  #    bootloader expects); copy it to the tester in BOOT mode, from Windows

  # Make (what the release workflow runs after building)
  python3 scripts/lpm_patch.py make VENDOR.bin BUILT.bin out.patch.json --name v1.0.0

  # Extract the checked vendor image to where the build scripts look for it
  python3 scripts/lpm_patch.py extract LPM-10C.zip

Standard library only, so it runs anywhere Python 3.8+ does.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

FORMAT = 'lpm-10c-patch/1'
VENDOR_NAME = 'LPM-10C_TX_V2.0.7_260907.bin'
VENDOR_SHA256 = '3bcc50065e16e93ddeff5fb9dce04f1b0447fd3f68a084803070978aef2bcaaa'
VENDOR_DIR = Path('firmware') / 'extracted' / 'LPM-10C' / 'LPM-10C_V2.0.7'  # relative to the repository root
MERGE_GAP = 16  # equal bytes shorter than this between two changes are folded into one range


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_vendor(path):
    """The vendor TX image, from the .bin itself or the FNIRSI .zip that contains it."""
    path = Path(path)
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.rsplit('/', 1)[-1] == VENDOR_NAME]
            if not names:
                raise SystemExit(f'{path}: no {VENDOR_NAME} inside')
            return archive.read(names[0])
    return path.read_bytes()


def ranges(old, new):
    """(offset, new bytes) for every run where new differs from old; old and new have the same length."""
    out, start, last = [], None, None
    for i, (a, b) in enumerate(zip(old, new)):
        if a == b:
            continue
        if start is not None and i - last <= MERGE_GAP:
            last = i
            continue
        if start is not None:
            out.append((start, new[start:last + 1]))
        start = last = i
    if start is not None:
        out.append((start, new[start:last + 1]))
    return out


def make(args):
    vendor, built = read_vendor(args.vendor), Path(args.built).read_bytes()
    if sha256(vendor) != VENDOR_SHA256:
        raise SystemExit(f'{args.vendor} is not the vendor V2.0.7 TX image (SHA-256 {sha256(vendor)})')
    if len(built) != len(vendor):
        raise SystemExit(f'{args.built} is {len(built)} bytes; the vendor file is {len(vendor)}')
    patch = {
        'format': FORMAT,
        'name': args.name,
        'output_name': VENDOR_NAME,
        'size': len(vendor),
        'vendor_sha256': VENDOR_SHA256,
        'output_sha256': sha256(built),
        'ranges': [{'offset': offset, 'bytes': data.hex()} for offset, data in ranges(vendor, built)],
    }
    Path(args.out).write_text(json.dumps(patch, indent=1) + '\n')
    changed = sum(len(r['bytes']) // 2 for r in patch['ranges'])
    print(f'Wrote {args.out}: {len(patch["ranges"])} ranges, {changed} bytes, output SHA-256 {patch["output_sha256"]}')


def apply(args):
    patch = json.loads(Path(args.patch).read_text())
    if patch.get('format') != FORMAT:
        raise SystemExit(f'{args.patch}: not a {FORMAT} patch')
    vendor = read_vendor(args.vendor)
    if sha256(vendor) != patch['vendor_sha256'] or len(vendor) != patch['size']:
        raise SystemExit(f'{args.vendor} is not the vendor file this patch needs (SHA-256 {sha256(vendor)}, '
                         f'expected {patch["vendor_sha256"]}). Use the official LPM-10C V2.0.7 update.')
    image = bytearray(vendor)
    for r in patch['ranges']:
        data = bytes.fromhex(r['bytes'])
        if r['offset'] < 0 or r['offset'] + len(data) > len(image):
            raise SystemExit(f'{args.patch}: range at {r["offset"]} is outside the file')
        image[r['offset']:r['offset'] + len(data)] = data
    if sha256(bytes(image)) != patch['output_sha256']:
        raise SystemExit('The patched image does not match the release; nothing was written.')
    out = Path(args.out) if args.out else Path(patch['output_name'])
    if out.is_dir():
        out = out / patch['output_name']
    out.write_bytes(bytes(image))
    print(f'Wrote {out} ({patch.get("name") or "patched"}), SHA-256 {patch["output_sha256"]}')
    if out.name != VENDOR_NAME:
        print(f'Rename it to {VENDOR_NAME} before copying it to the tester: the bootloader checks the name.')


def extract(args):
    vendor = read_vendor(args.vendor)
    if sha256(vendor) != VENDOR_SHA256:
        raise SystemExit(f'{args.vendor} does not hold the vendor V2.0.7 TX image (SHA-256 {sha256(vendor)})')
    out = Path(args.out) / VENDOR_NAME
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(vendor)
    print(f'Wrote {out}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('apply', help='rebuild a released image from the official update')
    p.add_argument('vendor', help='the FNIRSI LPM-10C.zip, or LPM-10C_TX_V2.0.7_260907.bin from it')
    p.add_argument('patch', help='the .patch.json from the release')
    p.add_argument('--out', help=f'output file or directory (default ./{VENDOR_NAME})')
    p.set_defaults(run=apply)
    p = sub.add_parser('make', help='write the patch between the vendor image and a build')
    p.add_argument('vendor')
    p.add_argument('built')
    p.add_argument('out')
    p.add_argument('--name', default='', help='release name recorded in the patch')
    p.set_defaults(run=make)
    p = sub.add_parser('extract', help='check the vendor image and put it where the build looks for it')
    p.add_argument('vendor', help='the FNIRSI LPM-10C.zip, or the .bin from it')
    p.add_argument('--out', default=str(VENDOR_DIR), help=f'directory (default {VENDOR_DIR}, from the repository root)')
    p.set_defaults(run=extract)
    args = parser.parse_args(argv)
    args.run(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Inspect the vendor TX image and decode its observed RGB565 resources offline."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib


def png(width, height, rgba):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def inspect(source, output):
    data = source.read_bytes()
    if not data.startswith(b'APP_LPM-10C_'):
        raise ValueError('Expected the vendor LPM-10C transmitter image')
    output.mkdir(parents=True, exist_ok=True)
    assets_dir = output / 'assets'
    assets_dir.mkdir(exist_ok=True)
    strings = [{'offset': hex(m.start()), 'text': m.group().decode('ascii')}
               for m in re.finditer(rb'[ -~]{5,}', data)]
    start, length, last = struct.unpack_from('<III', data, 0x20)
    if start + length != last + 1 or last >= len(data):
        raise ValueError('Header ranges do not match the observed container structure')
    resources = []
    # Signature observed in this release; these candidates are validated by size
    # and visual inspection, not assumed to describe all firmware resources.
    for match in re.finditer(rb'\x00\x10....\x01\x1b', data, re.DOTALL):
        offset = match.start()
        _, width, height, _ = struct.unpack_from('<4H', data, offset)
        end = offset + 8 + width * height * 2
        if not (1 <= width <= 320 and 1 <= height <= 320 and end <= last + 1):
            continue
        pixels = bytearray()
        for (value,) in struct.iter_unpack('<H', data[offset + 8:end]):
            pixels.extend(((value >> 11) * 255 // 31,
                           ((value >> 5) & 63) * 255 // 63,
                           (value & 31) * 255 // 31,
                           0 if value == 0x31a7 else 255))
        index = len(resources)
        name = f'asset-{index:02d}-{offset:06x}.png'
        (assets_dir / name).write_bytes(png(width, height, pixels))
        resources.append({'index': index, 'offset': hex(offset), 'width': width,
                          'height': height, 'file': name, '_pixels': pixels})
    cell_w, cell_h, columns = 180, 245, 4
    sheet_w = cell_w * columns
    sheet_h = cell_h * ((len(resources) + columns - 1) // columns)
    sheet = bytearray(bytes((36, 40, 48, 255)) * sheet_w * sheet_h)
    for asset in resources:
        x0 = (asset['index'] % columns) * cell_w + (cell_w - asset['width']) // 2
        y0 = (asset['index'] // columns) * cell_h + (cell_h - asset['height']) // 2
        pixels = asset.pop('_pixels')
        for y in range(asset['height']):
            for x in range(asset['width']):
                source_offset = (y * asset['width'] + x) * 4
                dest_offset = ((y0 + y) * sheet_w + x0 + x) * 4
                if pixels[source_offset + 3]:
                    sheet[dest_offset:dest_offset + 4] = pixels[source_offset:source_offset + 4]
    if resources:
        (output / 'asset-contact-sheet.png').write_bytes(png(sheet_w, sheet_h, sheet))
    manifest = {'source': str(source), 'sha256': hashlib.sha256(data).hexdigest(),
                'size': len(data), 'container_name': data[:32].split(b'\0')[0].decode(),
                'header_fields_observed': {'payload_start': hex(start), 'payload_length': hex(length),
                                           'last_payload_byte': hex(last)},
                'initial_sp': hex(struct.unpack_from('<I', data, start)[0]),
                'reset_vector': hex(struct.unpack_from('<I', data, start + 4)[0]),
                'resources': resources,
                'limitations': ['Resource header identification is specific to this release.',
                                'No bootloader acceptance, checksum or signature validation was performed.',
                                'RGB565 transparency key inferred from static display routine.']}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / 'strings.json').write_text(json.dumps(strings, indent=2) + '\n')
    print(json.dumps({'resources': len(resources), 'manifest': str(output / 'manifest.json')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, default=Path('firmware/analysis'))
    args = parser.parse_args()
    inspect(args.source, args.output)

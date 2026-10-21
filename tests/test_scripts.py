"""Host tests for the hardware helper scripts; no probe or tester needed."""
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

import build_patched_image  # noqa: E402
import lpm_patch  # noqa: E402
import swd_identify  # noqa: E402
import swd_sram_dump  # noqa: E402
import uart_baud  # noqa: E402
import uart_log  # noqa: E402

# A plausible OpenOCD session; register values are constructed, not captured from a device.
SAMPLE_LOG = '''Open On-Chip Debugger 0.12.0
Info : J-Link V9 compiled ...
Info : VTarget = 3.300 V
Info : SWD DPIDR 0x2ba01477
LPM dbg_id 0x88584555
LPM flash_ob 0x03fffffe
LPM flash_wrp 0xffffffff
LPM uid 0x11223344,0x55667788,0x99aabbcc
LPM ucid 0x00000001,0x00000002,0x00000003,0x00000004
LPM option_bytes 0x00ff00ff,0x00ff00ff,0x00ff00ff,0x00ff00ff,0x00ff00ff
LPM boot_vectors error Failed to read memory at 0x08000000
LPM app_vectors error Failed to read memory at 0x0800a000
'''


class SwdIdentifyTests(unittest.TestCase):
    def test_parse_output_reads_words_errors_and_dpidr(self):
        reads, dpidr = swd_identify.parse_output(SAMPLE_LOG)
        self.assertEqual(dpidr, 0x2BA01477)
        self.assertEqual(reads['uid'], [0x11223344, 0x55667788, 0x99AABBCC])
        self.assertIn('0x08000000', reads['boot_vectors'])

    def test_dbg_id_decodes_n32g455_with_512k_flash_and_144k_sram(self):
        # SRAM 8 -> 144 KB, DEV_NUM_L 5, FLASH 8 -> 512 KB, DEV_NUM_H 4, DEV_NUM_M 5, revision 0x55.
        d = swd_identify.decode_dbg_id(0x88584555)
        self.assertEqual((d['model'], d['flash_kb'], d['sram_kb'], d['revision']), ('N32G455', 512, 144, '0x55'))

    def test_dbg_id_reports_unknown_model(self):
        self.assertEqual(swd_identify.decode_dbg_id(0x00000000)['model'], 'unknown')

    def test_flash_ob_levels(self):
        self.assertEqual(swd_identify.decode_flash_ob(0x03FFFFFC)['read_protection'], 'L0 (unprotected)')
        self.assertEqual(swd_identify.decode_flash_ob(0x03FFFFFE)['read_protection'], 'L1')
        self.assertEqual(swd_identify.decode_flash_ob(0x83FFFFFE)['read_protection'], 'L2')
        self.assertTrue(swd_identify.decode_flash_ob(0x03FFFFFD)['option_byte_error'])

    def test_summary_flags_unreadable_flash_and_vendor_vector_match(self):
        reads, dpidr = swd_identify.parse_output(SAMPLE_LOG)
        summary = swd_identify.summarize(reads, dpidr)
        self.assertEqual(summary['flash_ob']['read_protection'], 'L1')
        self.assertIn('error', summary['app_vectors'])
        reads['app_vectors'] = [0x20011298, 0x0800A355]
        self.assertTrue(swd_identify.summarize(reads, dpidr)['app_vectors']['matches_vendor_v2_0_7'])
        self.assertTrue(any('L1' in line for line in swd_identify.report_lines(summary)))

    def test_openocd_script_only_reads(self):
        # Guards the safety property: no command that writes memory, touches flash, halts or resets.
        forbidden = re.compile(r'^\s*(mww|mwh|mwb|write_memory|load_image|program|flash|halt|reset(?!_config)|resume|'
                               r'step|n32\.cpu\s+(mw|write|arp_reset|arp_halt)|stm32|n32g)', re.IGNORECASE)
        for tcl in (swd_identify.TCL, swd_sram_dump.TCL):
            for number, line in enumerate(tcl.read_text().splitlines(), 1):
                code = line.split('#', 1)[0]
                self.assertIsNone(forbidden.search(code), f'{tcl.name} line {number}: {line}')
            self.assertIn('read_memory', tcl.read_text())


class SramDumpTests(unittest.TestCase):
    def test_parse_chunks_little_endian_and_errors(self):
        chunks, errors = swd_sram_dump.parse_chunks(
            'LPM sram 0x20000000 11223344,aabbccdd\nLPM sram 0x20001000 error read_memory: failed\n')
        self.assertEqual(chunks[0x20000000], bytes.fromhex('44332211ddccbbaa'))
        self.assertIn('failed', errors[0x20001000])

    def test_finds_cust_info_by_magic_and_skips_blank_records(self):
        image = bytearray(swd_sram_dump.SRAM_BYTES)
        record = bytearray(range(200))
        record[0xA0:0xA2] = (0x9718).to_bytes(2, 'little')
        image[0x2CF0:0x2CF0 + 200] = record
        image[0x8000 + 0xA0:0x8000 + 0xA2] = (0x9718).to_bytes(2, 'little')  # magic in an otherwise zero block
        self.assertEqual(swd_sram_dump.find_cust_info(bytes(image)), [0x20002CF0])


class UartLogTests(unittest.TestCase):
    def test_printable_score_separates_text_from_noise(self):
        self.assertEqual(uart_log.printable_score(b''), 0.0)
        self.assertGreater(uart_log.printable_score(b'12 POE: AT class 4\r\n'), 0.95)
        self.assertLess(uart_log.printable_score(bytes(range(128, 256))), 0.05)

    def test_line_stamper_joins_split_reads_and_crlf(self):
        s = uart_log.LineStamper()
        self.assertEqual(s.feed(b'12 LENG: Link ', 't1'), [])
        self.assertEqual(s.feed(b'speed 1000M\r', 't2'), [])
        self.assertEqual(s.feed(b'\nnext', 't3'), ['t3 12 LENG: Link speed 1000M'])
        self.assertEqual(s.flush('t4'), ['t4 next'])
        self.assertEqual(s.flush('t5'), [])

    def test_line_stamper_escapes_binary_bytes(self):
        self.assertEqual(uart_log.LineStamper().feed(b'a\xffb\n', 't'), ['t a\\xffb'])


def uart_samples(text, baud, sample_rate, idle_bits=3, pin=1):
    """Synthesise sampled 8N1 UART levels on one FT232H pin, with idle time between bytes."""
    bits = []
    for byte in text:
        bits += [1] * idle_bits + [0] + [(byte >> i) & 1 for i in range(8)] + [1]
    count = int(len(bits) * sample_rate / baud)
    return bytes((bits[int(i * baud / sample_rate)] << pin) | 0xFD for i in range(count))


class UartBaudTests(unittest.TestCase):
    def measure(self, baud, sample_rate, chunk=448):
        samples = uart_samples(b'12 LENG: Link speed 1000M\r\n13 POE: AT class 4\r\n' * 8, baud, sample_rate)
        runs = []
        for start in range(0, len(samples), chunk):
            runs += uart_baud.runs_in_chunk(samples[start:start + chunk], 0x02)
        bit = uart_baud.estimate_bit_time(runs, sample_rate)
        return uart_baud.nearest_standard(sample_rate / bit)

    def test_recovers_common_rates_from_chunked_samples(self):
        for baud, rate in ((115200, 1000000), (921600, 1000000 * 8), (57600, 1000000), (9600, 200000)):
            best, error = self.measure(baud, rate)
            self.assertEqual(best, baud)
            self.assertLess(error, 0.03)

    def test_runs_drop_partial_edges(self):
        self.assertEqual(uart_baud.runs_in_chunk(bytes([2, 2, 0, 0, 0, 2, 2]), 0x02), [(False, 3)])
        self.assertEqual(uart_baud.runs_in_chunk(b'', 0x02), [])

    def test_too_few_transitions_gives_no_estimate(self):
        self.assertIsNone(uart_baud.estimate_bit_time([(False, 9)] * 5, 1e6))


class ReleasePatchTests(unittest.TestCase):
    """lpm_patch.py on synthetic images: no vendor firmware needed."""

    def setUp(self):
        self.vendor = bytes(range(256)) * 16
        built = bytearray(self.vendor)
        built[10:14] = b'\xaa' * 4
        built[20] ^= 0xFF  # a separate range: the equal bytes between are not included
        built[3000:3002] = b'\x00\x01'
        self.built = bytes(built)
        self.patch = {'format': lpm_patch.FORMAT, 'name': 't', 'output_name': lpm_patch.VENDOR_NAME,
                      'size': len(self.vendor), 'vendor_sha256': lpm_patch.sha256(self.vendor),
                      'output_sha256': lpm_patch.sha256(self.built),
                      'ranges': [{'offset': o, 'bytes': d.hex()} for o, d in lpm_patch.ranges(self.vendor, self.built)]}

    def apply(self, vendor, patch):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'v.bin').write_bytes(vendor)
            (Path(tmp) / 'p.json').write_text(json.dumps(patch))
            lpm_patch.main(['apply', str(Path(tmp) / 'v.bin'), str(Path(tmp) / 'p.json'), '--out', tmp])
            return (Path(tmp) / lpm_patch.VENDOR_NAME).read_bytes()

    def test_ranges_cover_every_change_and_nothing_else(self):
        self.assertEqual([(o, len(d)) for o, d in lpm_patch.ranges(self.vendor, self.built)],
                         [(10, 4), (20, 1), (3000, 2)])

    def test_patch_holds_no_vendor_byte(self):
        self.assertEqual(lpm_patch.vendor_bytes(self.vendor, self.patch['ranges']), [])

    def test_apply_rebuilds_the_exact_image(self):
        self.assertEqual(self.apply(self.vendor, self.patch), self.built)

    def test_apply_refuses_another_vendor_file(self):
        with self.assertRaises(SystemExit):
            self.apply(self.vendor[:-1] + b'\x00', self.patch)

    def test_vendor_image_is_found_inside_the_nested_fnirsi_zip(self):
        import io
        import zipfile
        inner = io.BytesIO()
        with zipfile.ZipFile(inner, 'w') as z:
            z.writestr('LPM-10C/LPM-10C_V2.0.7/' + lpm_patch.VENDOR_NAME, self.vendor)
        with tempfile.TemporaryDirectory() as tmp:
            outer = Path(tmp) / 'download.zip'
            with zipfile.ZipFile(outer, 'w') as z:
                z.writestr('folder/LPM-10A.zip', b'not this one')
                z.writestr('folder/LPM-10C.zip', inner.getvalue())
            self.assertEqual(lpm_patch.read_vendor(outer), self.vendor)

    def test_apply_refuses_a_patch_whose_result_does_not_match(self):
        self.patch['output_sha256'] = '0' * 64
        with self.assertRaises(SystemExit):
            self.apply(self.vendor, self.patch)


class PatchedImageTests(unittest.TestCase):
    def setUp(self):
        if not build_patched_image.VENDOR.exists():
            self.skipTest('vendor image not extracted')
        self.vendor = build_patched_image.VENDOR.read_bytes()

    def test_label_patch_changes_only_the_label_bytes(self):
        patched = build_patched_image.apply_patches(self.vendor, build_patched_image.PATCHES['label-length-uppercase'])
        changed = [i for i in range(len(self.vendor)) if self.vendor[i] != patched[i]]
        self.assertEqual(changed, [0x68FD9, 0x68FDA, 0x68FDB, 0x68FDC, 0x68FDD])
        self.assertEqual(patched[0x68FD8:0x68FDF], b'LENGTH\x00')
        build_patched_image.check_container(patched)

    def test_auto_off_patch_changes_one_instruction_and_the_labels(self):
        patched = build_patched_image.apply_patches(self.vendor, build_patched_image.PATCHES['auto-off-5x'])
        changed = [i for i in range(len(self.vendor)) if self.vendor[i] != patched[i]]
        self.assertEqual([i for i in changed if i < 0x68000], [0x6EC7, 0x7B58, 0x7B60, 0x7B61, 0x7B62, 0x7B63, 0x7B64,
                                                              0x7B68, 0x7B69, 0x7B6A, 0x7B6B, 0x7B6C])
        # Thumb ADDS Rd, Rn, #imm3 (0x1C00 | imm3 << 6): r0 = r0 + 5 instead of + 1.
        self.assertEqual(int.from_bytes(patched[0x6EC6:0x6EC8], 'little'), 0x1C00 | 5 << 6)
        self.assertEqual(patched[0x7B50:0x7B70], b'OFF\x00\x16\xea\x05\x001min\x00\x00\x00\x002min\x00\x00\x00\x00'
                                                 b'3min\x00\x00\x00\x00')
        build_patched_image.check_container(patched)

    def test_branch_encoder_matches_the_vendor_call(self):
        self.assertEqual(build_patched_image.thumb_branch(0xA3DE, 0x800A3C0 - build_patched_image.BASE, link=True),
                         self.vendor[0xA3DE:0xA3E2])

    def test_extension_grows_the_header_length_and_keeps_the_rest(self):
        patched = build_patched_image.apply_patches(self.vendor, build_patched_image.PATCHES['extended-hook'])
        self.assertEqual(struct.unpack_from('<III', patched, 0x20), (0x1000, 0x68478, 0x69477))
        build_patched_image.check_container(patched, self.vendor)
        changed = [i for i in range(len(self.vendor)) if self.vendor[i] != patched[i]]
        self.assertTrue(all(i < 0x30 or 0xA3DE <= i < 0xA3E2 or 0x68FD8 <= i < 0x68FDF or 0x6944C <= i < 0x69478
                            for i in changed))
        self.assertEqual(patched[0x6944C:0x6944E], b'\x01\xa2')  # adr r2, fmt (word at 0x69454)
        self.assertTrue(patched[0x69454:].startswith(b'%d Current FW Version: %s +ext\r\n\x00'))
        with self.assertRaisesRegex(ValueError, 'outside the length'):
            build_patched_image.check_container(patched[:0x10] + b'X' + patched[0x11:], self.vendor)

    def test_menu_list_places_compiled_code_and_redirects_three_calls(self):
        try:
            build_patched_image.find_clang()
        except SystemExit:
            self.skipTest('clang not installed')
        patches = build_patched_image.patch_list('menu-list')
        patched = build_patched_image.apply_patches(self.vendor, patches)
        build_patched_image.check_container(patched, self.vendor)
        code = patches[-1][2]
        length = struct.unpack_from('<I', patched, 0x24)[0]
        self.assertEqual(length, 0x6844C + len(code))
        self.assertLess(0x1000 + length, 0x6A000)
        draw = build_patched_image.thumb_branch(0x6A7E, build_patched_image.PAYLOAD_END, link=True)
        self.assertEqual(patched[0x6A7E:0x6A82], draw)  # lpm_menu_draw is the first function
        self.assertEqual(patched[0x6A9E:0x6AA2],
                         build_patched_image.thumb_branch(0x6A9E, build_patched_image.PAYLOAD_END, link=True))
        self.assertNotEqual(patched[0x4802:0x4806], self.vendor[0x4802:0x4806])

    def test_home_ui_stays_within_the_vendor_file_size(self):
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        patches = build_patched_image.patch_list('home-ui')
        patched = build_patched_image.apply_patches(self.vendor, patches)
        build_patched_image.check_container(patched, self.vendor)
        offset, length, last = struct.unpack_from('<III', patched, 0x20)
        # A 438,272-byte image installed only partly on the tester; the vendor size is the proven limit.
        self.assertEqual(len(patched), len(self.vendor))
        self.assertLessEqual(offset + length, 0x6A000)
        self.assertGreater(length, 0x6844C)
        self.assertEqual(patched[0x1000:0x4802], self.vendor[0x1000:0x4802])

    def test_added_code_cannot_grow_the_file_past_the_vendor_size(self):
        end = build_patched_image.FLASH_LIMIT
        with self.assertRaisesRegex(ValueError, 'outside'):
            build_patched_image.apply_patches(self.vendor, [(end - 2, bytes(4), b'\x01' * 4, 'one word too long')])
        patched = build_patched_image.apply_patches(self.vendor, [(end - 4, bytes(4), b'\x01' * 4, 'last word')])
        self.assertEqual(len(patched), len(self.vendor))
        build_patched_image.check_container(patched, self.vendor)

    def test_dead_code_check_finds_live_references(self):
        import check_dead_code
        vendor = self.vendor
        # In the vendor image the menu routines are still called, so the ranges must NOT look dead.
        for low, high in check_dead_code.DEAD_RANGES.values():
            self.assertTrue(check_dead_code.find_references(vendor, low, high),
                            f'{low:#x}-{high:#x} reported unreachable in the vendor image')
        # Routines the patches never touch must stay reachable in a patched image too.
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        patched = build_patched_image.apply_patches(vendor, build_patched_image.patch_list('home-ui'))
        for low, high in ((0x08018568, 0x080185BC), (0x08018D48, 0x08018DAC)):  # fill_rect, draw_string
            self.assertTrue(check_dead_code.find_references(patched, low, high))
        # After the redirects the replaced routines are unreachable, apart from BLs the patches
        # pointed at added code placed there, which the vendor image tells apart.
        for low, high in check_dead_code.DEAD_RANGES.values():
            self.assertEqual(check_dead_code.find_references(patched, low, high, vendor), [])
        self.assertTrue(check_dead_code.find_references(patched, *build_patched_image.DEAD_RANGES[-1]))

    def test_dead_code_check_sees_the_glyph_table_base(self):
        import check_dead_code
        # The blitter forms the glyph table base 0x080708F8 and indexes from it, so in the vendor image
        # the reused table must show those references (and the missing guard).
        table = check_dead_code.DEAD_RANGES['CJK glyph table (blitter patched to bx lr)']
        kinds = {(kind, where) for kind, where, _ in check_dead_code.find_references(self.vendor, *table)}
        self.assertIn(('pc-relative address', 0x08018DDA), kinds)
        self.assertIn(('pc-relative address', 0x08018E78), kinds)

    def test_ascii_drawer_callers_are_all_reviewed(self):
        import check_dead_code
        name = 'CJK glyph table (blitter patched to bx lr)'
        b = build_patched_image
        routine, reviewed, _ = check_dead_code.CALLERS[name]
        callers = {offset + b.BASE for offset in range(check_dead_code.CODE_START, check_dead_code.CODE_END, 2)
                   if check_dead_code.branch_target(self.vendor, offset) == routine}
        self.assertEqual(callers, reviewed, 'the review of the ASCII drawer callers no longer matches the image')
        # A new call into the drawer, from added code, makes the table count as reachable again.
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        patched = bytearray(b.apply_patches(self.vendor, b.patch_list('home-ui')))
        self.assertEqual(check_dead_code.analyse(bytes(patched), self.vendor)[name], [])
        site = 0x69FF0
        patched[site:site + 4] = b.thumb_branch(site, routine - b.BASE, link=True)
        kinds = [kind for kind, *_ in check_dead_code.analyse(bytes(patched), self.vendor)[name]]
        self.assertIn('unreviewed caller', kinds)

    def test_link_status_takes_the_qc_test_slot(self):
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        b = build_patched_image
        patched = b.apply_patches(self.vendor, b.patch_list('home-ui'))
        for name, (site, vendor_target) in b.LINK_STATUS_CALLS.items():
            self.assertEqual(self.vendor[site:site + 4], b.thumb_branch(site, vendor_target - b.BASE, link=True), name)
            self.assertNotEqual(patched[site:site + 4], self.vendor[site:site + 4], name)
        # Key table entry 19: state 8 only, now M (key 4) pressed (3) -> event 0x0B.
        entry = 0x08026264 - b.BASE + 19 * 8
        self.assertEqual(struct.unpack_from('<IBBB', self.vendor, entry), (0x100, 1, 8, 0x0B))
        self.assertEqual(struct.unpack_from('<IBBB', patched, entry), (0x100, 4, 3, 0x0B))
        # Entries 2 to 5, UP, DOWN, LEFT and RIGHT short presses -> events 2 to 5 (lpm_home_key): home, and now
        # state 8, to scroll and to change tab. The held (repeating) LEFT and RIGHT entries stay home only.
        for index, key, event in ((2, 3, 2), (3, 2, 3), (4, 5, 4), (5, 1, 5)):
            entry = 0x08026264 - b.BASE + index * 8
            self.assertEqual(struct.unpack_from('<IBBB', self.vendor, entry), (0x004, key, 3, event))
            self.assertEqual(struct.unpack_from('<IBBB', patched, entry), (0x104, key, 3, event))
        # No other entry gives state 8 an UP or DOWN event first.
        for index in range(43):
            mask, key, press, event = struct.unpack_from('<IBBB', patched, 0x08026264 - b.BASE + index * 8)
            if mask & 0x100 and key in (2, 3):
                self.assertIn(index, (2, 3))
        # The 1 s tick skips the QC count command: bne -> b, same offset.
        self.assertEqual((self.vendor[0x1A30C:0x1A30E], patched[0x1A30C:0x1A30E]), (b'\x04\xd1', b'\x04\xe0'))
        home = (ROOT / 'src' / 'firmware_patch' / 'home_ui.c').read_text()
        self.assertIn('{"Link status", &lpm_icon_ethernet, 4}', home)
        self.assertNotIn('QC test', home)

    def test_vendor_drawing_routines_branch_to_the_theme(self):
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        b = build_patched_image
        patched = b.apply_patches(self.vendor, b.patch_list('home-ui'))
        for name, entry in b.THEME_ROUTINES.items():
            at = entry - b.BASE
            h1, h2 = struct.unpack_from('<HH', patched, at)
            self.assertEqual((h1 & 0xF800, h2 & 0xD000), (0xF000, 0x9000), f'{name}: no b.w at {entry:#x}')
            self.assertNotEqual(patched[at:at + 4], self.vendor[at:at + 4], name)
        # The tool title draws the home list names: draw_string at 0x0800F3F8 is redirected.
        site = b.TOOL_TITLE_CALL
        self.assertEqual(self.vendor[site:site + 4], b.thumb_branch(site, 0x8018D48 - b.BASE, link=True))
        self.assertNotEqual(patched[site:site + 4], self.vendor[site:site + 4])

    def test_theme_map_keys_are_not_theme_colours(self):
        """lpm_theme_colour passes the theme's own colours first, so a vendor colour equal to one would never be
        mapped, and every map target must be a theme colour so a second mapping leaves it alone."""
        theme = (ROOT / 'src' / 'lpm_ui' / 'lpm_theme.h').read_text()
        palette = {}
        for name, r, g, b in re.findall(r'#define (LPM_C_\w+) LPM_RGB565\((0x\w+), (0x\w+), (0x\w+)\)', theme):
            r, g, b = int(r, 16), int(g, 16), int(b, 16)
            palette[name] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        source = (ROOT / 'src' / 'firmware_patch' / 'theme.c').read_text()
        pairs = re.findall(r'\{(0x[0-9A-F]{4}), (LPM_C_\w+)\}', source)
        self.assertEqual(len(pairs), 17)
        listed = re.search(r'palette\[\] = \{([^}]*)\}', source).group(1)
        for key, target in pairs:
            self.assertNotIn(int(key, 16), palette.values(), key)
            self.assertIn(target, palette)
            self.assertIn(target, listed, f'{target} is a map target but not in the pass-through palette')

    def test_vendor_screens_open_themed_in_the_emulator(self):
        try:
            import unicorn  # noqa: F401
            build_patched_image.find_tool('ld.lld')
        except (ImportError, SystemExit):
            self.skipTest('unicorn, clang or lld not installed (run with .venv/bin/python)')
        import preview_vendor as V
        b = build_patched_image
        image = b.apply_patches(self.vendor, b.patch_list('home-ui'))
        sram = (ROOT / 'tests' / 'fixtures' / 'sram-sanitized.bin').read_bytes()
        ping = V.TOOLS.index('ping')
        tester = V.VendorTester(image, sram)
        tester.uc.mem_write(V.MENU_STATE, bytes([2, 4 + ping]))
        self.assertIsNone(tester.run(V.open_targets(image)[ping], 4 + ping))
        self.assertEqual(tester.drain(), [])
        # The vendor greys are gone: the frame body, inner boxes, label boxes and black background are themed.
        self.assertFalse(set(tester.fb) & {0x2105, 0x31A7, 0x4A69, 0x0000})

    def test_chinese_pictures_are_removed(self):
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        b = build_patched_image
        patched = b.apply_patches(self.vendor, b.patch_list('home-ui'))
        for site in b.COMPANY_NAME_PICTURE:  # About: picture 0x16 is set up as the third argument
            self.assertEqual(self.vendor[site - 6:site - 4], b'\x16\x22')  # movs r2, #0x16
            self.assertEqual(self.vendor[site:site + 4], b.thumb_branch(site, 0x800F422 - b.BASE, link=True))
            self.assertEqual(patched[site:site + 4], b'\x00\xbf\x00\xbf')
        site = b.RESET_WARNING_PICTURE
        self.assertEqual(self.vendor[site:site + 4], b.thumb_branch(site, 0x800F412 - b.BASE, link=True))
        self.assertNotEqual(patched[site:site + 4], self.vendor[site:site + 4])

    def test_link_status_scenarios_in_the_emulator(self):
        try:
            import unicorn  # noqa: F401
            build_patched_image.find_tool('ld.lld')
        except (ImportError, SystemExit):
            self.skipTest('unicorn, clang or lld not installed (run with .venv/bin/python)')
        import preview_link_status
        b = build_patched_image
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'image.bin'
            image.write_bytes(b.apply_patches(self.vendor, b.patch_list('home-ui')))
            argv = sys.argv
            sys.argv = ['preview_link_status.py', '--image', str(image), '--out', tmp]
            try:
                self.assertEqual(preview_link_status.main(), 0)
            finally:
                sys.argv = argv

    def test_freed_pictures_are_refused_and_the_others_still_draw(self):
        try:
            import unicorn  # noqa: F401
            build_patched_image.find_tool('ld.lld')
        except (ImportError, SystemExit):
            self.skipTest('unicorn, clang or lld not installed (run with .venv/bin/python)')
        import preview_menu
        b = build_patched_image
        sram = (ROOT / 'tests' / 'fixtures' / 'sram-sanitized.bin').read_bytes()
        table = struct.unpack_from('<24I', sram, 0xF0)  # the picture table, initialised at boot
        tester = preview_menu.Tester(b.apply_patches(self.vendor, b.patch_list('home-ui')), sram)
        from unicorn.arm_const import UC_ARM_REG_R0
        for index in range(24):
            tester.call(0x0800E8E0, index)  # the getter
            expected = 0 if index in b.FREED_PICTURES else table[index]
            self.assertEqual(tester.uc.reg_read(UC_ARM_REG_R0), expected, f'picture {index}')
        for index in b.FREED_PICTURES:  # the drawer returns for NULL: not one pixel
            tester.call(0x0800F422, 60, 60, index, 1)
            self.assertFalse(any(tester.fb), f'picture {index} drew something')
        tester.call(0x0800F422, 120, 160, 15, 1)  # the About logo, 108 x 18, is kept
        self.assertGreater(sum(1 for pixel in tester.fb if pixel), 108 * 18 // 4)
        # Nothing the patches add or reuse may overlap a picture still in use.
        kept = [(p, p + 8 + struct.unpack_from('<HH', self.vendor, p - b.BASE + 2)[0]
                 * struct.unpack_from('<HH', self.vendor, p - b.BASE + 2)[1] * 2)
                for i, p in enumerate(table) if i not in b.FREED_PICTURES and i != 14]
        for offset, _, new, note in b.patch_list('home-ui'):
            low, high = offset + b.BASE, offset + b.BASE + len(new)
            for start, end in kept:
                self.assertFalse(low < end and start < high, f'{note} overlaps a kept picture at {start:#x}')

    def test_chinese_is_removed_and_the_language_kept_english(self):
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        b = build_patched_image
        patched = b.apply_patches(self.vendor, b.patch_list('home-ui'))
        self.assertEqual(patched[b.CJK_BLITTER:b.CJK_BLITTER + 4], bytes.fromhex('704700bf'))  # bx lr; nop
        for site, vendor_target in [(s, 0x8010288) for s in b.LANGUAGE_LOAD_CALLS] + [(b.LANGUAGE_DEFAULTS_CALL,
                                                                                         0x801B340)]:
            self.assertEqual(self.vendor[site:site + 4], b.thumb_branch(site, vendor_target - b.BASE, link=True))
            self.assertNotEqual(patched[site:site + 4], self.vendor[site:site + 4])
        for site in b.LANGUAGE_TO_CHINESE:
            self.assertEqual((self.vendor[site:site + 2], patched[site:site + 2]), (b'\x02\x20', b'\x01\x20'))
        # The defaults routine reuses r0 = 2 for Auto Off, so it is wrapped rather than edited.
        self.assertEqual(patched[0x1233C:0x123A6], self.vendor[0x1233C:0x123A6])

    def test_cust_info_write_is_skipped_when_unchanged(self):
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        patched = build_patched_image.apply_patches(self.vendor, build_patched_image.patch_list('home-ui'))
        writer = 0x080104B8
        for site in build_patched_image.CUST_INFO_STORE_CALLS:
            self.assertEqual(self.vendor[site:site + 4],
                             build_patched_image.thumb_branch(site, writer - build_patched_image.BASE, link=True))
            self.assertNotEqual(patched[site:site + 4], self.vendor[site:site + 4])
        # The restore-defaults path keeps calling the writer directly, and the flash driver is untouched.
        self.assertEqual(patched[0x737E:0x7382], self.vendor[0x737E:0x7382])
        self.assertEqual(patched[0x7000:0x7680], self.vendor[0x7000:0x7680])

    def test_subset_font_covers_every_character_the_screens_draw(self):
        import gen_subset_font
        sources = [p for p in sorted((ROOT / 'src' / 'firmware_patch').glob('*.c')) if p.name != 'subset_font.c']
        needed = gen_subset_font.characters_used(sources) | gen_subset_font.BASELINE | {' '}
        generated = (ROOT / 'src' / 'firmware_patch' / 'subset_font.c').read_text()
        match = re.search(r"Characters: '(.*)'", generated)
        self.assertIsNotNone(match, 'subset_font.c has no character list; rerun gen_subset_font.py')
        present = set(match.group(1).replace("\\'", "'").replace('\\\\', '\\'))
        self.assertEqual(needed - present, set(),
                         'subset_font.c is stale: rerun python3 scripts/gen_subset_font.py')

    def test_reused_sections_stay_inside_the_cleared_ranges(self):
        import check_dead_code
        self.assertEqual(tuple(check_dead_code.DEAD_RANGES.values()), build_patched_image.DEAD_RANGES,
                         'build_patched_image.DEAD_RANGES and check_dead_code.DEAD_RANGES disagree')
        try:
            build_patched_image.find_tool('ld.lld')
        except SystemExit:
            self.skipTest('clang or lld not installed')
        patches = build_patched_image.patch_list('home-ui')
        reused = [(o, new) for o, _, new, note in patches if 'reused' in note]
        self.assertTrue(reused, 'no section was placed in the reclaimed ranges')
        for offset, blob in reused:
            address = offset + build_patched_image.BASE
            self.assertTrue(any(low <= address and address + len(blob) <= high
                                for low, high in build_patched_image.DEAD_RANGES),
                            f'{address:#x} + {len(blob)} escapes the cleared ranges')

    def test_added_code_must_not_need_linking(self):
        try:
            clang = build_patched_image.find_clang()
        except SystemExit:
            self.skipTest('clang not installed')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'extern.c'
            source.write_text('void vendor(void);\nvoid hook(void) { vendor(); }\n')
            with self.assertRaisesRegex(ValueError, 'relocations'):
                build_patched_image.compile_added_code(source)
        self.assertTrue(clang)

    def test_rejects_unsafe_patches(self):
        apply = build_patched_image.apply_patches
        with self.assertRaisesRegex(ValueError, 'length'):
            apply(self.vendor, [(0x68FD8, b'Length', b'Len', 'shorter')])
        with self.assertRaisesRegex(ValueError, 'outside'):
            apply(self.vendor, [(0x69FFE, bytes(4), b'\x01' * 4, 'past the vendor file size')])
        with self.assertRaisesRegex(ValueError, 'overlaps'):
            apply(self.vendor, [(0x1B640, self.vendor[0x1B640:0x1B642], b'\x00\xbf', 'UID binding')])
        with self.assertRaisesRegex(ValueError, 'overlaps'):
            apply(self.vendor, [(0x69360, self.vendor[0x69360:0x69364], b'\x00' * 4, 'region table')])
        with self.assertRaisesRegex(ValueError, 'vendor bytes'):
            apply(self.vendor, [(0x68FD8, b'Lxngth', b'LENGTH', 'wrong base')])


if __name__ == '__main__':
    unittest.main()

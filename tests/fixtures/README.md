# Test fixtures

`sram-sanitized.bin` is the 144 KB SRAM image (`0x20000000`–`0x20023FFF`) the
emulator previews (`scripts/preview_menu.py`, `scripts/preview_link_status.py`)
and `tests/test_scripts.py` load before running firmware code.

It is **not** a dump of a tester. It was made by running every emulator preview
and test against a real dump while recording the SRAM bytes the code reads
before writing them, then keeping only those bytes (everything else is zero):

| Address | Bytes | Contents |
| --- | --- | --- |
| `0x20000018` | 40 | vendor menu label pointers (into the firmware image) |
| `0x20000118` | 44 | vendor picture table pointers |
| `0x20000150` | 2 | system state and menu selection |
| `0x20000178` | 5 | GUI state bytes |
| `0x200001A0` | 2 | text colour |
| `0x20000214` | 6 | Ethernet MAC: the example `02:00:00:00:00:01`, not a real unit's |
| `0x20000224` | 2 | frame length (zero) |
| `0x20002D95` | 1 | settings record `+0xA5`, language 1 (English) |
| `0x20002FCC` | 10 | LCD controller command bytes |
| `0x2000F0AC`… | | parts of the frame buffer, all zero |

No settings, calibration, network addresses or logs from a real unit are in it.
If a change makes the emulator read other SRAM, a preview faults or fails its
checks; extend the fixture with the value the vendor start-up code would put
there, not with bytes from a device.

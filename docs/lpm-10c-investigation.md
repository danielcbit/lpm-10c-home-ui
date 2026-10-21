# LPM-10C firmware investigation

Investigation date: 2026-10-19. All device access was read-only. No firmware was flashed and no protection bits or settings were changed.

## Findings

| Question | Result |
| --- | --- |
| Installed firmware | Originally V2.0.3 (owner report and UART), hardware V2.1.0. **Updated to vendor V2.0.7 on 2026-10-19** from Windows; see [bootloader analysis](lpm-10c-bootloader-analysis.md#successful-update-to-v207-2026-10-19-windows). Now running the patched build `label-length-uppercase` (V2.0.7 with `Length` → `LENGTH`, [details](lpm-10c-bootloader-analysis.md#first-modified-image-label-length-uppercase)) |
| USB identity | NATIONS N32G45x Storage, VID:PID 19F5:5720 |
| Firmware extracted from device | Not obtained: mounted drive exposes no firmware file; raw reads denied by host |
| Vendor firmware | Downloaded by owner; LPM-10C transmitter V2.0.7 and receiver V3.0.1 extracted |
| UI modification | Text, bitmap, and color changes are technically plausible; bootloader acceptance and recovery remain unverified |

## Device observations

The USB product and generic serial strings are `N32G45x Storage` and `N32G45x`. They suggest a Nations/Nsing N32G45x MCU; the exact chip remains unverified. The USB `bcdDevice` field is 0x0200, which does not identify the installed application version.

`/Volumes/BOOT` was mounted from `/dev/disk15` and reported 444 KiB total. It contained only a zero-byte `README.TXT`. No serial interface attributable to the tester was observed. This is consistent with an update bootloader, rather than a filesystem containing an accessible firmware backup. Device paths can change after reconnection.

Read-only opens of `/dev/disk15` and `/dev/rdisk15` returned permission denied. This establishes a host restriction, not device readout protection. Even a successful image of this volume would need analysis before calling it a firmware backup.

Evidence: USB descriptors, directory listing, mount.

## Package and identity

The original archive is preserved in `firmware/vendor/`. Its SHA-256 is `6dfea6eef1f8ec5c4630b63690d3730fe6f2a76a9a4b094008a871de3d28c746`.

The outer archive contains separate LPM-10A and LPM-10C archives. Only LPM-10C was extracted into `firmware/extracted/LPM-10C/`.

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| `LPM-10C_TX_V2.0.7_260907.bin` | 434176 | `3bcc50065e16e93ddeff5fb9dce04f1b0447fd3f68a084803070978aef2bcaaa` |
| `LPM-10RX_V3.0.1_260807.bin` | 32768 | `3a712aaffce97ab33c6b80e68438252ac6def1dbf379d224adb5b79b74ff9a86` |

The TX binary contains `V2.0.7` at file offset `0x8c24`, `V2.1.0` at `0x8c38`, and `LPM-10C` at `0x8c4c`. Its embedded hardware version matches the owner's report. This is supporting evidence, not a verified installation test. The RX is a separate component; its version number is not the transmitter hardware version.

The release notes mention battery detection, cable docking, calibration, length units, and PoE fixes. The TX filename includes `260907`, while the previously observed vendor listing date was 2026-08-07; the filename is recorded without treating it as a verified build date.

## Container and executable

Observed TX header fields:

| File offset | Value | Interpretation |
| --- | --- | --- |
| `0x00` | `APP_LPM-10C_V2.0.7_260907.bin` | Embedded name |
| `0x20` | `0x1000` | Apparent payload start |
| `0x24` | `0x6844c` | Apparent payload byte length |
| `0x28` | `0x6944b` | Apparent inclusive final payload offset |
| `0x1000` | `0x20011298` | Initial stack pointer |
| `0x1004` | `0x0800a355` | Thumb reset vector |

The range fields agree with each other and the final nonzero payload data; the remainder is zero padding. Static reset-handler and GUI disassembly support an application load address of `0x0800a000`, mapping file offsets to flash addresses by adding `0x08009000`.

Readable code, strings, and images demonstrate that substantial parts of the application are unencrypted. They do not establish whether the bootloader checks a checksum, signature, filename, or other metadata. The bootloader itself is not included in this application payload. No update acceptance checks were executed. A later [bootloader interface analysis](lpm-10c-bootloader-analysis.md) found no checksum or signature anywhere in the file, and found UID-based copy protection in the application.

## UI resources and code

Recovered asset contact sheet

The offline decoder recovered **21 RGB565 bitmap candidates**, visually checked as UI artwork. They include RJ45 icons, the FNIRSI logo, PING/PoE labels, Wi-Fi levels, settings/speed icons, and a wiring illustration. The bitmap renderer skips color `0x31a7`, supporting its interpretation as a transparency key.

Useful targets, with file offsets:

| Target | Offset |
| --- | --- |
| FNIRSI logo, 108 × 18 pixels | `0x25ad1` |
| Settings icon, 52 × 57 pixels | `0x4f4f9` |
| Speed icon, 57 × 52 pixels | `0x50c29` |
| Wiring illustration, 165 × 227 pixels | `0x52e01` |
| `Cable Test` | `0x68fbc` |
| `Length` | `0x68fd8` |
| `SETTING` | `0x69004` |
| Settings labels including `Language`, `Volume`, `About` | `0x69324` onward |

The menu routine at inferred address `0x0800f648` uses fixed RGB565 colors, coordinate data, and calls to bitmap and text renderers. Examples include white `0xffff` and green `0x07e0`. The icon helper at `0x0800f422` reads width/height from header offsets +2/+4 and image data at +8; the pixel renderer is at `0x080183ec`. ASCII text rendering at `0x08018d48` advances through characters and calls a glyph routine.

This supports a compiled embedded GUI with patchable resources. Searches found no literal `lvgl`, `FreeRTOS`, or `RT-Thread` identifiers; absence of those strings does not identify or rule out a framework or RTOS.

## Practical modification options

- **Icons/logo:** Replace pixels while preserving dimensions, header, byte count, and RGB565 encoding. Existing asset boundaries provide concrete patch locations.
- **English labels:** Equal-length replacements are the simplest offline experiment. Longer labels require validating storage and renderer references. Some text is stored separately from its references; broad search-and-replace is unsuitable.
- **Colors:** Patch selected color-setting instructions or data after tracing their use. The transparency key needs special handling.
- **Layout/navigation:** Requires deeper control-flow and coordinate-table analysis. No source project was supplied.

No modified update image was generated. Flash acceptance, a working recovery method, and a backup of this unit's firmware/settings/calibration remain unresolved. A vendor application update does not provide that backup.

## Reproduce and continue

Run from the repository root with Python 3; the resource decoder uses only the standard library:

```sh
python3 scripts/inspect_firmware.py \
  firmware/extracted/LPM-10C/LPM-10C_V2.0.7/LPM-10C_TX_V2.0.7_260907.bin
```

Outputs: `firmware/analysis/manifest.json`, `strings.json`, individual PNGs in `assets/`, and the contact sheet. Static disassembly records the inspected code; hashes preserve binary identities. The generated ELF is an analysis-only wrapper, never a firmware update file.

REA was available for this pass. Archive inspection evidence: `ev_0f3d4b28a7aeb1da74e28a2537923a1d552401cf5aca23fd951611bcdfe719fa`. Wrapper overview evidence: `ev_ca41a7ccd2994379c6df3e7abbef1d518674fa2b1f95f7ae9021874884e2e21a`, retained in `evidence/rea-tx-overview.json`. REA's firmware/Binwalk integration requires Linux, and its raw BIN loader rejected this format. Hopper recognized the ELF wrapper but found only one procedure; focused UI conclusions therefore use separately recorded Capstone disassembly and Python byte inspection, not recovered source or observed execution. The analysis session was closed.

Next steps are to choose a specific UI edit, build and review an offline patch, and establish bootloader validation and recovery before device writes. Full device extraction may require an authorized raw-volume capture or, if USB provides no readback, board identification and SWD access with protection status checked without changing it. A later [board inspection](lpm-10c-board-inspection.md) found a labelled SWD/UART header next to the MCU.

## Vendor references

[FNIRSI firmware listing](https://www.fnirsi.com.cn/firmware/p/2.html) distinguishes model and hardware generations. [FNIRSI update requirements](https://www.fnirsi.com/pages/manuals-firmwares?category=environmental-home) specify Windows 10/11 and warn against updates from macOS. [N32G455 family information](https://www.nsing.com.sg/product/General/cortexm4/N32G455/) documents flash protection capabilities, not this tester's actual protection state.

## Follow-up UI and capability study

See [UI and feature proposal](lpm-10c-ui-and-feature-proposal.md), [network protocol evidence](lpm-10c-network-analysis.md), and [sourced MCU/controller capabilities](lpm-10c-hardware-capabilities.md). These refine the interface proposal and distinguish proven ARP/ICMP/DHCP support from unverified TCP functionality. No firmware patches or device writes were made.

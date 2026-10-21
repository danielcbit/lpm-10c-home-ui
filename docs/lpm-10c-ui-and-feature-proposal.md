# LPM-10C UI and feature proposal

This is a design and feasibility study, not modified firmware. Its first-priority items are now implemented as a portable, host-tested UI module; see [implementation notes](lpm-ui-implementation.md). Static findings concern downloaded TX **V2.0.7**; the owner's installed firmware is **V2.0.3**, hardware **V2.1.0**. No device writes were performed.

## Recommended navigation

Use a single vertical list as the home screen. Show six rows, each about 36 pixels high, with a small icon, a clear name, a selection highlight and a visible position count. Suggested order: Cable map, Cable length, Cable trace, Port flash, PoE voltage, Link status, Ping monitor, IP scan, Settings. Remember the last selection when returning from a test. Avoid carousel page changes and horizontal scrolling labels.

Use Up/Down for selection and M to enter/start. Preserve the established Back and power behavior; keep Left/Right for changing fields or tabs. Confirm button event mapping before patching it. Avoid overloading existing long-press calibration and power actions. At list boundaries stop by default; accelerated repeat can move through a long list. The concept currently wraps for convenient exploration; that detail should be decided before firmware work.

The alternative groups Cable tools, Network tools, PoE and Settings. It scales better if many diagnostics are added but adds a navigation step. Prefer the direct list for the present feature count.

## Graphics and typography

Replace large shaded illustrations with a consistent set of native-size 20–24 pixel monochrome or two-tone icons. Use cable/pins, ruler, trace probe, port/flash, bolt, Ethernet, latency line, host list and settings symbols. Keep text labels visible. Wireless signal bars are a poor metaphor for wired connectivity and do not imply the tester has Wi-Fi hardware.

Use one readable pixel-aligned font, typically 16-pixel body text, 12-pixel status labels and 24–32-pixel measurement numerals. Align decimal places and always display units. Use one selection accent, strong foreground contrast and explicit words/symbols for faults; color alone should not carry a failure state. Avoid gradients, glossy buttons, gauges without useful scales and arbitrary bitmap enlargement. The panel's physical resolution cannot be improved by a firmware theme.

Artwork can be designed as vectors on a computer and rasterized to the exact target size or implemented with primitive drawing. The current renderer is not an SVG engine. If existing bitmap paths are retained, preserve RGB565 format and account for transparency key `0x31a7`.

## Suggested result screens

| Screen | Replacement elements | Data and limitation |
| --- | --- | --- |
| Ping | Latest RTT, min/mean/max, loss count; last 30–120 probes as a line/strip plot, explicit gaps for timeouts | Existing ICMP path; show RTT variation rather than claiming voice-quality jitter or bandwidth |
| Cable map | Eight numbered local/remote pins with straight/crossed lines; OPEN/SHORT/CROSS labels adjacent to affected pins | Render reported wiring states; no inference of certified cable category |
| Length | Large total length; aligned per-pair bars if independent pair values are actually available | Otherwise show one calibrated estimate; no fabricated per-pair data or analog TDR waveform |
| PoE | Large voltage and min/max trend with a labeled scale; pair/mode text where the existing measurement exposes it | Not a loaded-power test or current measurement unless supported by hardware |
| IP scan | Scrollable IP/MAC list, observed state, scan progress, cancel; select a host to ping | ARP results are not a physical topology map; names require additional resolution |
| DHCP | DISCOVER → OFFER → REQUEST → ACK timeline with durations; server, mask, router and lease state | Distinguish OFFER-only fallback from a confirmed lease |
| Link health | Step list: physical link, address source, ARP gateway, ping; show each observed result | DNS and Internet tests require their own implementation; failed ping alone does not prove Internet failure |

The interactive concept uses **simulated** values and includes direct-list/grouped alternatives plus ping, wiremap, length, PoE and scan examples. It demonstrates layout, not exact panel rendering or functioning device diagnostics.

## Firmware evidence for this design

The existing menu routine at inferred flash `0x0800f648` draws four slots per page. Its coordinate data at TX file `0x1d0c4` contains icon centers `(60,50), (180,50), (60,190), (180,190)` followed by labels at `(61,106), (179,106), (61,243), (179,243)`. Other nearby coordinates extend past y=300. This supports a portrait layout consistent with a 240×320 working design; the physical panel/controller dimensions have not been independently measured.

The LCD routines write through memory-mapped addresses `0x60000000` and `0x60020000`; the text, bitmap, filled-region and line routines already provide useful building blocks. Firmware primitives are sufficient for lists and simple plots without introducing a large graphics framework. Exact display controller and bus timing remain unconfirmed.

The 21 extracted RGB565 assets contain **293,612 bytes of pixel data**, approximately 68.7% of the 427,084-byte application payload. Four 154×120 signal images account for 147,840 bytes (144.4 KiB). Sixteen 24×24 monochrome icons need only 1,152 bytes of bitmap data, before metadata and rendering code. This is a resource-budget opportunity, not existing free space: absolute references and layout must be reconciled, rather than deleting bytes from the binary.

A full 240×320 RGB565 framebuffer needs 153,600 bytes, exceeding even 144 KiB of SRAM before the rest of the application. Prefer direct/partial drawing, two-row selection redraws, a bounded plot sample ring and small tile buffers. For example, 120 samples at 8 bytes/sample cost 960 bytes; the exact record layout is a future implementation choice.

## Network extension priorities

[Detailed network evidence](lpm-10c-network-analysis.md) establishes DM9051, uIP-derived ARP, ICMP echo, ARP host discovery and direct UDP DHCP. A complete reusable TCP/socket stack is not established.

1. **First:** list home screen, clear wiremap, ping history/loss, improved IP/MAC results and gateway checklist. These reuse the most established paths.
2. **Next:** DHCP inspector with ACK-state correctness, additional DHCP option parsing and duplicate-IP warnings using ARP. ARP silence cannot guarantee an unused address.
3. **Then:** LLDP/CDP switch/port/VLAN advertisement decoding, after receive-filter checks; DNS reachability/latency using a new bounded UDP parser; traceroute with TTL and ICMP time-exceeded handling. Availability depends on peer responses.
4. **Later:** TCP service reachability and HTTP report transfer after locating or adding TCP support and budgeting buffers. USB report export needs an application USB implementation; bootloader mass storage is not that implementation.

Do not promise gigabit throughput testing, Wi-Fi analysis or certification from these changes. The DM9051 packet interface is 10/100, and MCU/SPI/software limits are lower still. The advertised gigabit link detection uses a separate Motorcomm gigabit PHY ([board inspection](lpm-10c-board-inspection.md)); it reports link speed, not throughput.

## MCU and wider platform use

The supported identification is the **Nations/Nsing N32G45x family**, not a confirmed exact SKU. Candidate family devices provide Cortex-M4F processing up to 144 MHz, up to 512 KB flash and different SRAM capacities, reaching 144 KB on some variants. Clock, SKU, RAM size and actual free memory remain unverified. See the [sourced hardware assessment](lpm-10c-hardware-capabilities.md).

The best wider uses are a portable network commissioning tool, repeatable connectivity checks, intermittent-link/latency monitoring, result summaries and low-rate trends through existing measurement paths. Possible application USB telemetry can make it a computer-connected test instrument. Extra ADC, DAC, CAN, SDIO or serial peripherals on a candidate MCU do not establish accessible pins, transceivers, protected inputs or sockets. Additional radios, bridging, general packet recording and scope-like functions are not supported by current evidence.

## Implementation boundary

A list menu changes rendering, selection and key handling; it is more involved than replacing icons. There is no source project yet. A practical path is to validate the host concept, map menu/input/test-state interfaces, implement a bounded replacement view and determine the bootloader's image validation and recovery method before any flash write. Preserve the original firmware and calibration/settings. Any code transplanted into reclaimed artwork space must preserve all live references and the update container's validated requirements.

Offline concept logic was exercised for both variants: selection, enter/back, graphs, wiremap, length and state retention. It has not been visually tested in a connected browser or on the tester. The original TX hash was verified unchanged after analysis.

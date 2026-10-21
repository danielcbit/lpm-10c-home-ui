# LPM-10C hardware capabilities and limits

Investigated 2026-10-19 using existing local captures and the vendor V2.0.7 transmitter image, then updated after a [visual board inspection](lpm-10c-board-inspection.md). No device writes or electrical probes were performed.

## Identity and confidence

| Claim | Evidence and confidence |
| --- | --- |
| Nations/Nsing N32G45x family | Strong inference: the connected USB device advertises `N32G45x Storage`, VID:PID `19F5:5720`; see capture. USB descriptors are firmware-controlled, so they are not a chip-marking inspection. |
| Exact MCU SKU, installed SRAM, clock | **N32G455, 512 KB flash, 144 KB SRAM (revision 0x11).** Read from `DBG_ID` = `0x8F584511` over SWD on 2026-10-19; the Cortex-M4 r0p1 core was detected. The marking is blacked out. The package looks like LQFP100, and the board has an 8 MHz crystal. The core clock is not measured. Flash read protection is level 1. See [board inspection](lpm-10c-board-inspection.md#swd-result-2026-10-19). |
| DAVICOM DM9051 in the ping path | Strong static evidence: TX offset `0xc634` says `PING: Network chip: DAVICOM DM9051`; offsets `0x1977f` onward contain DM9051 ID/register diagnostics. See strings. Confirmed by the board marking `DM9051AN`. |
| Motorcomm gigabit PHY | The board has a YT8521SC with 24-pin magnetics. The firmware's length module drives a PHY named YT8531, including a `Link speed 1000M` message. The naming mismatch is unresolved. See [board inspection](lpm-10c-board-inspection.md). |

The manufacturer groups N32G451, N32G452, N32G455 and N32G457 under N32G45x. Their RAM capacities differ: G451 48–96 KB; G452/G455 80–144 KB; G457 144 KB. All list up to 512 KB flash. Only G457 in this matrix includes integrated Ethernet. The family name alone therefore does not establish either RAM capacity or an integrated MAC. [Official family matrix](https://www.nationstech.com/product/general/n32g/n32g45x/).

## Representative processing resources

For a **candidate G455**, the official specification lists Cortex-M4F with floating point and DSP instructions, up to 144 MHz, up to 512 KB flash and 144 KB SRAM. It provides SPI, I2C, UART, USB Full Speed **device**, CAN, SDIO, timers/DMA, ADCs, DACs and comparators; SWD/JTAG and UART/USB bootloader support are documented. Those are chip capabilities, not evidence of accessible signals on this tester. [Official G455 specifications](https://www.nationstech.com/product/general/n32g/n32g45x/n32g455/).

G457 likewise offers up to 144 MHz, 512 KB flash and 144 KB SRAM, with an additional 10/100 Ethernet interface. Its availability does not establish that this tester uses it. [Official G457 specifications](https://www.nationstech.com/product/general/n32g/n32g45x/n32g457/).

The previous image mapping places application data at flash `0x0800a000`, length `0x6844c`. If that mapping and a 512 KiB internal-flash device are correct, the remaining address range above the image is only **56,244 bytes (54.9 KiB)**. This is a conditional arithmetic estimate, not verified free storage: calibration, settings, reserved sectors, update checks and alignment may consume it. Current RAM use is unknown.

## Ethernet constraints

DM9051 integrates a 10/100 PHY/MAC and 16 KB packet SRAM; its SPI slave supports up to 50 MHz. It is **not a gigabit packet interface**. SPI overhead, firmware scheduling and host MCU speed further limit useful throughput below the Ethernet line rate. If this board uses the G455's ordinary SPI, its documented maximum is 36 MHz, but actual clock and wiring remain unknown. [DAVICOM DM9051 datasheet](https://www.davicom.com.tw/pddocs/DM9051%28I%29-12-MCO-DS-P01_03302015.pdf), [G455 specifications](https://www.nationstech.com/product/general/n32g/n32g45x/n32g455/).

Gigabit link detection uses separate circuitry: a Motorcomm YT8521SC gigabit PHY, found by [board inspection](lpm-10c-board-inspection.md). Ping traffic does not pass through it. How it connects to the MCU has not been traced. Do not turn a link-speed indication into a claimed throughput measurement.

## Useful platform extensions

These are engineering proposals, conditional on recovering the relevant routines and memory budget:

- Retain ping histories, packet loss and latency summaries in bounded ring buffers; draw sparklines using integer coordinates.
- Run repeatable cable/PoE/network checks and keep a compact result history, subject to flash endurance and available storage.
- Add USB serial telemetry or result export by implementing another application USB class. USB bootloader storage alone does not prove application USB support, and USB device mode does not support attaching arbitrary USB peripherals.
- Implement small protocol diagnostics over existing Ethernet frames: DHCP option display, DNS queries, LLDP decoding or selected TCP service checks. Each needs protocol implementation and testing; it is not enabled merely by the DM9051.
- Investigate low-rate analog trend recording only through the tester's existing validated measurement paths. Spare ADC/DAC/CAN/UART peripherals do not imply usable sockets, transceivers, isolation or protected inputs.

A full framebuffer is unnecessary for a list interface. For illustration, a 240 × 320 RGB565 framebuffer costs 153,600 bytes, exceeding even 144 KiB SRAM before application state. Partial redraws, small line/tile buffers, monochrome icons and bounded data histories are a better fit. That display size is an example for memory budgeting, not a newly verified panel measurement.

Avoid promising gigabit throughput benchmarking, a general packet recorder, a transparent bridge, high-speed oscilloscope operation or extra radio capability from the MCU specification alone. Physical topology, storage and measurement electronics determine those capabilities.

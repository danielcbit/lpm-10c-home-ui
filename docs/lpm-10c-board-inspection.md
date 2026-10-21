# LPM-10C transmitter board inspection

The owner photographed the opened transmitter on 2026-10-19. This was a visual inspection only. Nothing was connected to the board, powered, probed or written.

The photos (`evidence/board-photos/`), UART captures and SWD dumps named below are kept by the author and are not part of the public repository.

## Board

The silkscreen reads `10_TX_V2.3_20260310`. Treat the date as a revision label, not a verified manufacturing date. The top side carries the MCU, Ethernet, PoE and power circuitry. The bottom side carries the analog multiplexers, the display connector and seven push buttons. See the top overview and bottom overview.

## Components

| Part | Marking | Photo | Likely role | Confidence |
| --- | --- | --- | --- | --- |
| Main MCU | Marking blacked out. The Nations logo and name are faintly visible | close-up | Application processor | **N32G455, 512 KB flash, 144 KB SRAM, revision 0x11**, from the `DBG_ID` register over SWD (2026-10-19) |
| MCU clock | 8.000 MHz crystal | top | External high-speed clock (HSE) | Marking read |
| Motorcomm YT8521SC | `YT8521SC VDTH01559 04800210D 3AMG56EMD` | close-up | **Gigabit** (10/100/1000) Ethernet PHY with a 25 MHz crystal. Probably the link-speed test | Marking read. Role inferred |
| JXD 2621X | `G2415S` | close-up | Ethernet magnetics with 24 pins, typical of gigabit (four-pair) modules | Marking read. Rating not checked against a datasheet |
| DAVICOM DM9051AN | `2536C X P233U.12` | close-up | 10/100 SPI Ethernet controller with its own 25 MHz crystal. The ping, scan and DHCP packet path | Marking read. Matches the firmware strings |
| HC4066 | `1A30841 TXD23 23E` | photo | Quad analog switch | Marking read. Role unknown |
| HC4051 ×3 | `5920018 YXD25 11F` | photo | 8-channel analog multiplexers. Probably select cable conductors for wiremap and length | Marking read. Role inferred |
| HC125 (TI) | `TI 64U CJ62 106` | photo | Quad 3-state buffer, next to a row of eight identical resistors coded `01A` | Marking read. Role unknown |
| V358A | `798 LQAX` | photo | Dual op-amp (358 type) | Marking read. Role unknown |
| onsemi NCP1095 | `NCP 1095 PWAL` | photo | PoE powered-device interface controller. Explains the `POE: AT class` and `app_poe_set_standar` strings | Marking read. Role inferred |
| MB6S ×2 | `MB6S` | top | Bridge rectifiers. The usual polarity-independent PoE input on two pair sets | Marking read |

## Debug and service connections

- **Labelled 6-pin header** next to the MCU: `SWCLK, GND, SWDIO, 3V3, RX, TX`. It provides SWD debug access and a UART. This is the route to identify the chip, check read protection and attempt a full flash backup.
- **5-wire cable** to a separate board, labelled `VBUS, DP, …, 3V, RESET` (the middle label is obscured). It is probably the USB connector board used by the `BOOT` mass-storage updater.

SWD is enabled, and flash read protection is set to **level 1**. See the [SWD result](#swd-result-2026-10-19) below.

## Corrections to earlier conclusions

- **The board has a gigabit PHY.** Earlier documents said gigabit link detection "may involve different circuitry, which has not been identified". That circuit is now identified: the YT8521SC.
  - The firmware has a matching driver in its length (`LENG`) module. It includes `Link speed 1000M`, a PHY soft reset that writes register 0 = `0x8000`, and set 1000/100 Mb/s and auto-negotiation messages.
  - Those strings name a **YT8531**, a related Motorcomm gigabit PHY, not the YT8521SC fitted here. Either the driver is shared or the part was substituted. This is unresolved.
- **The DM9051 is still the 10/100 packet path.** It has its own 10/100 PHY. The N32G45x family has at most a 10/100 MAC, and only on the G457. A link-speed indication from the gigabit PHY therefore does not imply gigabit throughput measurement. How the YT8521SC connects to the MCU (MDIO, data interface) has not been traced.
- **The package is about 25 pins per side, consistent with LQFP100.** That fits the LCD on the external memory bus at `0x60000000`. The pins were not counted exactly.

## Why the MCU marking is removed

Blanking the main chip is a common anti-cloning measure. It hides the exact part and supplier, and it lets the maker substitute compatible chips without anyone noticing. It does not stop identification: the logo is still faintly visible, the firmware names `N32G45x`, and the device-ID register can be read over SWD.

## Next steps: UART console, then read-only SWD identification

Both steps only read from the tester. Power the tester from its own battery.

**1. UART console** (`scripts/uart_log.py`). The firmware contains many debug messages such as `%d LENG: Link speed 1000M` and `%d POE: AT class %d`. They probably appear on the header's TX pin.
- With the FT232H: connect board TX to D1 (RXD), and GND to GND. Leave board RX and 3V3 unconnected.
- The rate is 1,000,000 baud: `uart_log.py --port ftdi://ftdi:232h/1 --baud 1000000`. Power-cycle the tester to capture the boot log.
- The port is opened read-only.
- **Findings so far (2026-10-19).** The line idles high, and it only transmits at power-on.
  - Captures at 9600 and 500000 baud were unreadable. `uart_baud.py` at a 1 MHz sample rate reported about 500000, but it had only about 2 samples per bit.
  - The 500000 capture (`evidence/uart/uart-20261019-130911-500000.bin`) is structured, not random: 71% of the bytes have bit 7 set, and sequences repeat.
  - I simulated sending the firmware's log strings at a range of rates and receiving them at 500000. The byte histogram matches the capture best when the real rate is about 0.95–0.99 Mbaud (cosine similarity 0.93; about 0.4 at 921600 and under 0.21 everywhere else from 250k to 2M). The likely rate is **1 Mbaud**.
- **Confirmed: 1,000,000 baud, 8N1.** The capture `evidence/uart/uart-20261019-131259-1000000.txt` (9,531 bytes) is clean text. It covers the end of a previous session, a power-off, two boots and menu navigation. Each line is `<uptime ms> <MODULE>: <message>`. What it shows:
  - **`Current FW Version: V2.0.3`.** This confirms the owner's installed version. Its application vectors will therefore probably differ from the V2.0.7 image.
  - **The DM9051 is present and answering:** `DM9051 id: 0x90510A46`, `DM9051_REG_1fh = 0x6`.
  - **Network:** the static IP settings and the MAC address are logged at boot. The MAC is in Davicom's OUI, `00:60:6E`.
  - **The gigabit PHY is answering MDIO.** `LENG: YT8531 reg 0 = 0x1140` is the IEEE default (auto-negotiation enabled, 1000 Mb/s speed bits). At boot the firmware powers it down (`set pwr_down 1`). The driver name is still YT8531, while the fitted part is a YT8521SC.
  - **The boot path:** `APP_Home_Check_BootFlag 0xFFFFFFFF` (the flag is unset/erased), then `Charge but not PWR_ON 1`, then `Batt_vol = 4136mV`.
  - **The main menu** is `Cable Test, SCAN, FLASH, Length`, at (60,50), (180,50), (60,190) and (180,190). The key events include `key0` (power), `key1`, `key3` and `key5`.
  - **Some lines are truncated or merged**, for example `stable_cnt=2, di  222000 HOM…`, and once with a few stray bytes. Each burst also appears twice. Whether this comes from the firmware's log buffer or from receive overruns is not known. Uptime stamps that don't go backwards within a burst suggest the firmware.
- Nothing was sent to the tester. The adapter's TX was not connected.

**2. SWD identification** (`scripts/swd_identify.py`, OpenOCD 0.12).

| J-Link 20-pin | Board header | Notes |
| --- | --- | --- |
| 7 (TMS/SWDIO) | SWDIO | |
| 9 (TCK/SWCLK) | SWCLK | |
| 4 (or any even pin 4–20) | GND | |
| 1 (VTref) | 3V3 | Sense only. First check that pin 1 does not output a voltage when nothing is connected; some clones drive it. Never connect pin 19 (5 V supply) |

The FT232H can serve as an alternative probe through OpenOCD's `ft232h-module-swd.cfg`. Wire D0 to SWCLK, D1 to SWDIO through about 470 Ω, and D2 to SWDIO directly.

The script reads only the following. The addresses come from the N32G45x Series User Manual V3.1.0:

| Register | Address | Gives |
| --- | --- | --- |
| `DBG_ID` | `0xE0042000` | Device number (0x452/0x455/0x457), flash = FLASH[19:16] × 64 KB, SRAM = (SRAM[31:28] + 1) × 16 KB, revision. The section header gives offset 0x04, but the register map puts `DBG_ID` at 0x00 and `DBG_CTRL` at 0x04 |
| `FLASH_OB` | `0x4002201C` | Bit 1 `RDPRT1` (L1), bit 31 `RDPRT2` (L2), bit 0 option-byte error |
| `FLASH_WRP` | `0x40022020` | Write-protection bits |
| UID / UCID | `0x1FFFF7F0` (96 bits) / `0x1FFFF7C0` (128 bits) | Unique chip IDs, readable over SWD |
| Option bytes | `0x1FFFF800`–`0x1FFFF813` | Raw RDP/USER/data/WRP bytes |
| Vector tables | `0x08000000`, `0x0800A000` | The second one, if readable, is compared with the vendor image (`0x20011298`, `0x0800A355`) |

**Interpreting the result:**
- **L0:** flash is readable over SWD, so a full backup is possible. The backup is the next step.
- **L1:** debug access to main flash is blocked. Registers and SRAM can still be reached. Returning to L0 **mass-erases all of main flash**, so do not do it. A backup would then have to come from the bootloader or from the application itself.
- **L2:** debug is disabled entirely, and SWD will not connect.

A failed connection can also mean bad wiring, or firmware that repurposes the SWD pins. The header has no reset line, so connect-under-reset is not available without adding a wire to NRST.

Decline any "unlock", "mass erase" or "recover" prompt from any tool.

## SWD result (2026-10-19)

The FT232H was used as the probe at 100 kHz. The run was read-only: no halt, reset, write or erase. The raw log and decoded JSON are in `evidence/swd/identify-20261019-150734.{log,json}`.

| Read | Value | Meaning |
| --- | --- | --- |
| SWD DPIDR | `0x2BA01477` | Standard Arm Cortex-M4 debug port. OpenOCD reports a Cortex-M4 r0p1 |
| `DBG_ID` | `0x8F584511` | Device 0x455, so **N32G455**. Flash nibble 8, so 512 KB; SRAM nibble 8, so 144 KB; revision 0x11 |
| `FLASH_OB` | `0x03FFFFFE` | `RDPRT1` set and `RDPRT2` clear, so **read protection level 1**. No option-byte error. Software watchdog. Data0/Data1 are 0xFF |
| `FLASH_WRP` | `0xFFFFFFFF` | No flash pages are write-protected |
| Option bytes | `0x00FFFF00 0x00FF00FF 0x00FF00FF 0x00FF00FF 0xFFFFFFFF` | RDP byte 0x00 (not the 0xA5 that means L0), so this agrees with L1. USER 0xFF. Data 0xFF/0xFF. WRP all 0xFF |
| UID, UCID | read failed | Blocked under L1 for this part, even though the option bytes next to them are readable |
| Vectors at `0x08000000`, `0x0800A000` | read failed | Main flash is closed to the debugger, as L1 implies |

What follows from this:
- **A full flash backup over SWD is not possible.** It is not possible to compare the installed V2.0.3 image with V2.0.7 over SWD, and neither is backing up calibration or settings stored in main flash.
- **Do not lower the protection to L0.** That mass-erases all of main flash, including the bootloader, the application and any calibration. It cannot be undone, and no copy of this unit's flash exists.
- **The FNIRSI `BOOT` mass-storage updater still works under L1.** Code running from flash can still program flash. It remains the only known write path, and acceptance of a modified image is still unverified.
- **Peripheral registers and all 144 KB of SRAM can be read over SWD** while the core runs, even though the UID cannot. `scripts/swd_sram_dump.py` used this to back up the settings record. See [bootloader analysis](lpm-10c-bootloader-analysis.md#settings-record-v203-ram-copy).

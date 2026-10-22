# LPM-10C update path: what the bootloader can check

Static analysis on 2026-10-19 of the downloaded TX **V2.0.7** image (SHA-256 `3bcc5006…aaa`). Nothing was written to the device.

The bootloader itself (`0x08000000`–`0x08009FFF`) **has not been seen**. The chip is at read-protection level 1 ([SWD result](lpm-10c-board-inspection.md#swd-result-2026-10-19)), so SWD cannot read it, and the vendor package does not include it. The conclusions below come from the update file and from what the application does on the bootloader's side of the interface.

To reproduce: `python3 scripts/inspect_boot_interface.py` writes `evidence/tx-boot-interface-disassembly.txt`. Addresses below are flash addresses: file offset + `0x08009000`.

## Summary

| Question | Finding | Confidence |
| --- | --- | --- |
| Is the update file signed, encrypted or checksummed? | **No.** It is a 4 KB header followed by the plain Thumb image. The header holds only the name, payload offset, length and last offset. CRC-32, the STM32/N32 hardware CRC, byte sums and word sums of the payload appear nowhere in the file. The rest of the header and the padding after the payload are zero | High for the file. A check hidden in the bootloader (for example on the name or the vector table) cannot be ruled out without seeing it |
| Can the bootloader detect a modified image? | Nothing in the file gives it a reference to compare content against. If the header stays consistent (`last = offset + length − 1`, name unchanged), a content change looks no different from a vendor release | **Confirmed (2026-10-19).** The patched image `label-length-uppercase` was accepted and runs; see [the first modified image](#first-modified-image-label-length-uppercase) |
| Does the application ever enter the bootloader? | No. It never jumps to `0x08000000` and never requests a system reset. The only `AIRCR` access is the standard NVIC priority setup. The bootloader runs first on every reset and decides on its own whether to start the application | High |
| Where does the device keep its state? | Three flash pages, listed below. The application reads and writes no other flash outside its own code | High for V2.0.7 |
| Is there copy protection? | **Yes, in the application.** The chip UID is bound to an info page in the bootloader area. If the check fails, `main()` hangs | High |

## Flash map

| Address | Owner | Contents |
| --- | --- | --- |
| `0x08000000`–`0x08008FFF` | Bootloader | Not readable (L1). Code and USB mass-storage updater |
| `0x08009000` (page) | Bootloader / application handshake | Words 0–1 are key material; words 2–5 hold the encrypted chip UID. A `_V2.` marker sits at `+0x6C`. See below |
| `0x0800A000`–`0x0807244B` | Application V2.0.7 | Vector table: SP `0x20011298`, reset `0x0800A355`. Payload length `0x6844C` |
| `0x0807F000` (page) | Bootloader and application | Boot flag `0x9718`. The bootloader sets it after an install, and the application sets it when it writes default settings. The application acts on it and erases it on the next boot. Otherwise `0xFFFFFFFF` |
| `0x0807F800` (page) | Application | **All settings**: a 200-byte `Cust_Info` record, valid when the half-word at `+0xA0` is `0x9718`. Also holds the cable-count init data (`APP_Home_Storage_CntInitData`). RAM copy at `0x20002CF0` in V2.0.7 |

The application only ever writes flash through these four routines:
- The info-page writer at `0x08019634`.
- BootFlag storage (`0x0800FFCC`) and BootFlag check (`0x080101B4`).
- `Cust_Info` storage (`0x080104B8`).

Each one unlocks flash with the standard keys `0x45670123` / `0xCDEF89AB`, erases one 2 KB page, programs words, locks, and reads back to compare.

## Copy protection in `main()`

Right after setting `VTOR = 0x0800A000`, `main()` (`0x08023204`) does the following:

1. **It calls `0x0802463C`.** This routine reads the 96-bit UID at `0x1FFFF7F0` and the info page at `0x08009000`.
   - **If word 0 is not erased** (as after an install), it rewrites the page in this order:
     - word 0 = `0xFFFFFFFF`
     - words 2–5 = the UID, encrypted with a custom routine (`0x0800AEF0`; constants `0xDB613AFD`, `0x249EC503`, `0x1B9A3255`), keyed from words 0–1
     - `_V2.` at `0x0800906C`
   - **On every boot**, it decrypts words 2–5 (`0x0800ABA4`) and compares them with the UID. If the comparison returns 1, `main()` loops forever.
2. **It calls `0x08016FF4`.** If the byte at `0x0800906E` is not `'2'`, `main()` loops forever.

The binding is created by the application on its first run after an install. That implies **the bootloader prepares the info page when it installs an application**: word 0 is no longer erased afterwards. A flash dump copied onto another chip would hang.

For a modified vendor image this changes nothing, because the code is kept as is. A from-scratch firmware does not need it. It must not damage the info page, though, because the bootloader may rely on it.

The cipher was not identified, and the role of word 1 is not resolved. The comparison helper returns +1 or −1, and only +1 hangs. That looks like a vendor bug, but it doesn't matter here.

## What this means for a firmware update

- **A failed application should be recoverable.** The bootloader runs before the application on every reset and is never modified by an update. Re-flashing the vendor V2.0.7 image through `BOOT` should therefore recover from a bad custom image. Two things are still unconfirmed:
  - **How the `BOOT` mode is entered.** Now answered: the M button is held at power-up, and the bootloader handles it (see Next steps, item 2).
  - **Whether the bootloader can also be entered when the application hangs early.**
- **Settings probably survive an update.** The settings and first-boot pages are at the top of flash, about 53 KB beyond the end of the V2.0.7 payload. A bootloader that erases only what it writes leaves them alone.
  - One supporting sign: the `BOOT` volume reports 444 KiB. That is less than the 468 KiB between `0x0800A000` and `0x0807F000`.
  - This is weak evidence, because FAT overhead is unknown.
  - If an update does erase them, the application writes defaults on the next boot (`Cust_Info_Init password error`). The cable-count init data would then need to be redone.
- **No separate factory calibration area was found.** The application reads no other flash. Any calibration is either in the 200-byte `Cust_Info` record or built into the code.

## Bootloader UART output

**The bootloader prints nothing on the header UART** (2026-10-19). The evidence is three captures:
- **Pulse detector:** two `uart_baud.py` runs at a 6 MHz sample rate during BOOT entry saw 0 low pulses. They sampled only about 7% of the time, so they would catch slow output, not a short burst at 1 Mbaud.
- **Full capture at 1 Mbaud:** `uart-20261019-195902-1000000.txt` was a continuous receive started at 19:59:02. The owner entered BOOT mode, then powered on normally. Nothing arrived until the application's boot at 19:59:29 (uptime 0), which proves the wiring worked.

So the bootloader's checks cannot be observed this way.

## First update attempt (2026-10-19, macOS): not applied

What was done: the owner entered `BOOT` mode with M plus USB and copied `LPM-10C_TX_V2.0.7_260907.bin` to the drive from macOS.

What happened:
- The tester kept showing `Firmware Upgrade`, with no progress and no reboot.
- After a power cycle it still ran V2.0.3.
- The host view of the drive afterwards showed the file (434,176 bytes, 20:07), `README.TXT`, and a `.fseventsd` folder that macOS had written at 20:06, before the file.
- The drive is FAT12, 458,752 bytes (448 KiB), with 444 KiB usable.

What this shows:
- **The failed attempt changed nothing that boots.** V2.0.3 still runs, so the bootloader did not erase the application before the transfer was complete or accepted.
- **The bootloader cannot buffer the whole file**, since 424 KiB exceeds the 144 KB SRAM. It must program sectors as they arrive, or recognise the file and only then program, or discard them.
- **Why it did nothing is unknown.** Candidates:
  - macOS writes (`.fseventsd`, possibly Spotlight or `._` files) arriving before or between the file's sectors.
  - A missing eject, if the bootloader acts on the SCSI eject command.
  - Silent rejection.
- **The vendor supports Windows 10/11 only** and warns that a Mac may brick the device. The next attempt should follow that.

## Successful update to V2.0.7 (2026-10-19, Windows)

What was done: the owner entered `BOOT` mode with M plus USB and copied the unchanged `LPM-10C_TX_V2.0.7_260907.bin` from Windows Explorer. The tester **rebooted by itself** when the copy finished.

The UART log `evidence/uart/uart-20261019-200527-1000000.txt` (boot at 20:28:11) shows:
- **`Current FW Version: V2.0.7`.** The application starts normally, so the UID copy-protection check passed after the bootloader's install.
- **The settings survived:**
  - `APP_Home_Cust_Info_Init Success`, so the record at `0x0807F800` is still valid.
  - `CNT: APP_CNT_InitData_Init 0 <= 221, size:16` matches the value seen before the update.
- **`APP_Home_Check_BootFlag 0x9718`.** Under V2.0.3 this read `0xFFFFFFFF` on every boot. **The bootloader therefore writes the `0x9718` flag at `0x0807F000` after an install.** The application acts on it (`APP_HOME_set_sysState 1` at 101 ms) and erases it. So the flag means "just updated", not only first boot. It also shows that the bootloader knows the top-of-flash layout, and that it leaves the `Cust_Info` page alone.
- **The first battery reading is 0 mV** (`Batt init: vol=0mV, level=20%`) while on USB. Two seconds later it reads 4200 mV. The release notes mention a boot battery-detection fix, so this is probably that code path. It is not a fault, though not confirmed.
- **The menus**, for reference:
  - Page 1: Cable Test / SCAN / FLASH / Length.
  - Page 2: QC Test / SPEED / POE / PING.
  - Page 3: IP SCAN / SETTING, plus two empty slots whose names print as garbage bytes (unset string pointers logged).

The update path is now established: **Windows, M held at power-up, copy the unchanged file, wait for the automatic reboot.** A macOS copy was ignored, and left the old application intact.



1. **Done (2026-10-19): `Cust_Info` backed up from SRAM.** Under L1, SRAM is readable over SWD while the core runs.
   - `scripts/swd_sram_dump.py` read all 144 KB with no errors. The snapshot contains the firmware's log buffer and task names, which shows it is real data.
   - The V2.0.3 record is at `0x20000CF0`, with an identical second copy at `0x20007598` (probably the heap buffer used by `Cust_Info_Init`).
   - Saved as `evidence/swd/sram-20261019-194429-cust_info-20000cf0.bin` (200 bytes).
   - This is the RAM image of the flash record taken while the tester ran. Any setting changed since boot would differ from flash. See [settings record](#settings-record-v203-ram-copy).
2. **`BOOT` mode is entered by holding the M button** while plugging in USB, or perhaps just while powering on (owner, 2026-10-19; which of the two is needed is not pinned down).
   - The application has no code that enters the bootloader, so this button check must be in the bootloader.
   - The UART log shows that power-off really removes the MCU's power: after `PWR_OFF state`, the next boot starts at uptime 0, via the power key or USB (`Charge but not PWR_ON`). Every power-up therefore passes through the bootloader first.
   - So recovery from a bad application image should not depend on that image. This is **inferred**; it has not yet been exercised with a broken image.
3. **Treat the first write as a vendor-image test.** V2.0.7 over V2.0.3 confirms the update path, the UID re-binding and settings retention, using the image the bootloader is known to accept.

## First modified image: `label-length-uppercase`

`python3 scripts/build_patched_image.py` builds `firmware/patched/label-length-uppercase/LPM-10C_TX_V2.0.7_260907.bin`. Its SHA-256 is `6c453bd7…d556`; `manifest.json` records both hashes and the patch.

**The change:** the main-menu label `Length` becomes `LENGTH` at file `0x68FD8` (flash `0x08071FD8`). That is **5 bytes**, and nothing else changes.

**Why this is safe:**
- **Nothing moves.** The scatter-load table (`0x0807235C`) shows that read-only data ends below `0x0807235C`. The menu's pointer table lives in the compressed RW initialisation data, which is copied to RAM at `0x20000000`. Its compressed bytes contain `bc 1f 07 08`, the address of `Cable Test`. A same-length string change therefore moves no address and touches no pointer.
- **The header is unchanged:** name, offset, length and padding are all as in the vendor file.
- **The build refuses unsafe patches.** `build_patched_image.py` rejects any patch that changes length, misses the vendor bytes, leaves read-only data, or touches the header, the vector table, the flash or copy-protection routines, or the scatter-load data. `tests/test_scripts.py` checks these rules.
- **The output keeps the vendor file name**, because the bootloader's file-name rules are unknown.

**Expected result:**
- Menu page 1 shows `LENGTH` at (180,190).
- The UART log shows `APP_GUI_Show_menu 3 at 180,190 LENGTH` and still `Current FW Version: V2.0.7`.
- **If the tester does not boot**, enter `BOOT` mode with M and install the vendor file again from Windows.

**Result (2026-10-19): installed and working.** The owner installed it from Windows the same way as the vendor update. The tester rebooted by itself, and menu page 1 shows `LENGTH`. UART `uart-20261020-000342-1000000.txt` (2026-10-20) confirms it:
- `Current FW Version: V2.0.7`, and the settings record loads normally (`APP_Home_Cust_Info_Init Success`, count data 221).
- `APP_GUI_Show_menu 3 at 180,190 LENGTH`, then `HOME: selete_fun:LENGTH`.
  - The owner saw `selete_fun:Length` here before the patch, but none of the saved captures includes that line. The same string at `0x08071FD8` is therefore used for the screen label and for both log lines.
- The startup sequence is the same as the vendor V2.0.7 boot.

This confirms that the bootloader accepts a modified application:
- It does not check a checksum, signature or hash of the image.
- The copy protection in `main()` still passes, because it binds to the info page and the UID, not to the image contents.
- The vendor file name and an unchanged header are enough. Whether other names or header lengths are accepted is still untested.

## First code change: `auto-off-5x`

`python3 scripts/build_patched_image.py auto-off-5x` builds `firmware/patched/auto-off-5x/LPM-10C_TX_V2.0.7_260907.bin`. Its SHA-256 is `bae32aee…9961`, and **17 bytes** differ from the vendor file.

**How Auto Off works** (`0x0800FEAC`, called from the main event loop at `0x0800FE66`):
- Each tick adds 1 to a halfword counter at `0x2000016C`.
- When the counter passes the limit for the setting, it calls `0x08011464(2, 0, 0)`, which powers off.
- The limits come from a halfword table at `0x0806F2E4`, indexed by `Cust_Info +0xA2`: 0, 300, 600, 900. The values suggest the tick is once per second.
- Setting 0 (OFF) returns before counting.
- `0x0800FF04` clears the counter. It is called from three places, presumably on key activity.

**The patch:**
- **Code:** `0x0800FEC6` `adds r0, r0, #1` (`40 1c`) becomes `adds r0, r0, #5` (`40 1d`). This is one 16-bit instruction, with no branch and no change in size.
- **Labels:** the setting labels change to match: `5min`/`10min`/`15min` become `1min`/`2min`/`3min`. They are inline strings at file `0x7B58`–`0x7B6D`, and the shorter ones are padded with NUL.
- **Menu:** the `LENGTH` label is kept.

**Expected result:**
- With Auto Off on `1min` and no key pressed, the tester powers off about 60 s after the last key press. Choosing `2min` should take about 120 s.
- The measured time also confirms the tick rate.
- On battery, the UART log should end with a `PWR_Off by reason …` line. The reason is probably not 1, which was the manual power-off.

**If the tester does not boot**, reinstall the vendor file through `BOOT` mode, or the `label-length-uppercase` image.

**Result (2026-10-20): works as predicted.** The capture is `uart-20261020-000949-1000000.txt`.
- **The image was freshly installed.** The first boot logs `APP_Home_Check_BootFlag 0x9718`. Settings and count data loaded normally, and the menu shows `LENGTH`.
- **The owner set Auto Off in SETTING**, then went back to the menu at 43590 ms (`key0`, `APP_HOME_set_sysState 2`). No key was pressed after that.
- **At 104000 ms the log shows `PWR_Off by reason 2`**, then `APP_Home_Cust_Info_Storage Success` and `PWR_OFF state`. That is 60.4 s after the last key press, the expected 1 minute.
  - This confirms the 1 s tick, so the vendor timeouts really are 5, 10 and 15 minutes.
  - It also confirms that a code patch is accepted and runs.
- **The renamed label shows on screen:** the owner saw `1min` in SETTING. The UART log does not print setting labels.
- **Reason 2 is Auto Off.** Reason 1 was the manual power-off.
- **The MCU stayed powered after the power-off**, printing `PWR_OFF state` every 2 s. Unlike a battery power-off, external power was presumably connected.
- **Empty slots on menu page 4** (`IP SCAN`, `SETTING`) log garbage names such as `\xbcy` and `\xf8{`. This is vendor behaviour: the patch does not touch the menu tables.

## Boundary test: `extended-hook`

`python3 scripts/build_patched_image.py extended-hook` builds `firmware/patched/extended-hook/LPM-10C_TX_V2.0.7_260907.bin`. Its SHA-256 is `d3eaf44b…b8da`, and **49 bytes** differ from the vendor file.

**The question:** does the bootloader install a payload longer than the vendor's, and does added code run? All earlier patches kept the vendor length, so they replaced bytes in place.

**The patch** (it also keeps the `LENGTH` label; Auto Off is back to vendor timing):
- **Header:** the length goes from `0x6844C` to `0x68478`, and the last offset from `0x6944B` to `0x69477`. The name, payload offset and file size (434176 bytes) are unchanged.
- **Added code and data:** 44 bytes are appended at flash `0x0807244C`, just after the vendor payload and in the same 2 KB flash page:
  ```
  0807244C  01 a2        adr  r2, fmt          ; fmt = 0x08072454
  0807244E  97 f7 b7 bf  b.w  0x0800A3C0       ; the firmware's snprintf
  08072454  "%d Current FW Version: %s +ext\r\n"
  ```
- **Hook:** at `0x080133DE`, the log task's `bl 0x0800A3C0`, which formats the version line, becomes `bl 0x0807244C`.
  - The hook only swaps the format string in `r2` and tail-calls the same function.
  - Stack arguments, return address and buffer size (60 bytes) are unchanged.

**Expected results:**

| Bootloader | Expected log |
| --- | --- |
| Installs the longer payload | The boot log shows `0 Current FW Version: V2.0.7 +ext`; the rest of the boot is unchanged |
| Rejects the header | The screen stays on `Firmware Upgrade` and the old app remains, as in the failed macOS attempt |
| Accepts the header but writes only the vendor length | The hook lands on erased flash and faults: no log after `APP_LOG_task Init success`, blank screen |

**Recovery:** in either failure, hold M at power-up and install the vendor file or `label-length-uppercase`. The bootloader runs before the app, so a faulting app cannot block `BOOT` mode.

**Result (2026-10-20): the longer payload is installed and the added code runs.** The capture is `uart-20261020-011227-1000000.txt`; the install itself was not logged.
- **At 01:13:54 the boot log shows `0 Current FW Version: V2.0.7 +ext`.** The text comes from the format string at `0x08072454`, past the vendor end, so the bootloader wrote the bytes beyond `0x0807244B`. It follows the header length rather than a fixed size.
- **The rest of the boot is identical to vendor V2.0.7:** settings and count data load (221), DM9051 and PHY initialise, and the menu shows `LENGTH`.
- **`BootFlag 0xFFFFFFFF`,** so this was not the first boot after the install; the first boot was not captured.
- **The earlier part of the capture** (uptime 31690–40956 ms) is the already-running app being powered off (`reason 1`) and on again. It contains no version line, so it does not show which image was running.

**What this opens up:**
- **The file can carry up to `0x6A000 − 0x6944C` = 2996 more bytes** without changing the file size.
- **Free flash after the app runs to the BootFlag page at `0x0807F000`,** about 50 KB. Using it needs a larger update file, and whether the bootloader accepts a different file size is still untested.
- **Added code can be reached from any call site** with a same-size `bl` redirect.

## Flash wear

**Updates are not a wear concern.**
- The N32G45x datasheet rates the main flash at **100,000 erase/write cycles with 10-year retention**.
- An update erases each application page once, so even 1,000 test installs use about 1 % of the rating.

**The page that wears fastest is the settings page.** Vendor code erases and reprograms `Cust_Info` (`0x0807F800`) at **every** power-off, including Auto Off, and does not first check whether anything changed:
- The storage routine is at `0x080104B8`.
- It allocates a buffer, erases the page (`0x08016E5C`), programs 204 bytes, and then compares 200 bytes with `0x0800A6A0`.
- At 10 power-offs a day, that is about 27 years to the rated count.

**This is fixed in `home-ui`** by `src/firmware_patch/cust_info.c`.
- `lpm_cust_info_store_if_changed` compares the RAM record at `0x20002CF0` with flash `0x0807F800` and calls the vendor writer only when they differ.
- **The comparison is the vendor's own test for a good write:** after programming, `0x08010560` verifies the same first `0xC8` bytes with the same memcmp (`0x0800A6A0`) and treats equal as success. If they already match, erasing and reprogramming cannot change the stored bytes.
- **Seven of the eight call sites are redirected.** `0x0801037E` is left alone: it is the restore-defaults path, which always changes the record, and it lies inside the protected flash-driver range.
- **The flash routines themselves are untouched.** Only the `BL` targets change.
- **How to confirm:** the vendor logs `APP_Home_Cust_Info_Storage` inside the writer, so a power-off with no settings change prints **no storage line at all**. Change a setting, power off, and the line comes back.

**Confirmed on the tester (2026-10-20).**

| Capture | Session | At `PWR_Off by reason 1` |
| --- | --- | --- |
| `uart-20261020-023537` | backlight mode changed in SETTING | `APP_Home_Cust_Info_Storage` + `Success` |
| `uart-20261020-024036` | no setting changed; menu browsed, screens 8/11/12 opened, cable-count tests run | **no storage line** |

The second capture also settles a worry about the fix being useless in practice: the firmware does **not** touch the settings record during ordinary use. A 33-second session that ran tests and moved through several screens left the record identical to flash, so the erase was skipped. Only a real settings change now costs an erase cycle.

## List menu: `menu-list`

`python3 scripts/build_patched_image.py menu-list` builds `firmware/patched/menu-list/LPM-10C_TX_V2.0.7_260907.bin`. It replaces the 2×2 grid, three pages of four items, with one scrolling list of the same ten items. It also keeps the `LENGTH` label. The new code is C in `src/firmware_patch/menu_list.c`.

**Vendor menu, as traced:**
- **State byte `0x20000151`** holds 4 + the selected item (4..13).
- **Label pointers** are in RAM at `0x20000018`, with ten entries.
- **Grid drawing, `0x0800F648(partial)`,** is called from `0x0800FA7E` (full draw, after the screen is cleared to black and the header tab is drawn by `0x0801B0A8`) and from `0x0800FA9E` (redraw on message 7).
  - Its only private state is `0x2000004A`.
  - It logs `APP_GUI_Show_menu` for every slot, including the two empty slots past item 10, which read beyond the label table. This explains the garbage names.
- **Grid navigation, `0x080180C0(action)`,** is called from `0x0800D802`.
  - Actions: 2 up, 3 down, 4 left, 5 right.
  - Each action moves within the 2×2 page or to the next or previous page, then posts message 7 through `0x0800E960(7, 0, 0)`.
- **Drawing routines:**
  - `fill_rect(x0, y0, x1, y1, colour)` at `0x08018568` writes the LCD at `0x60020000`.
  - `draw_string(x, y, w, h, size, text)` at `0x08018D48` takes size 12 or 16.
  - Text uses the colours at `0x200001A0` (foreground) and `0x200001A2` (background).
- **Screen:** 240×320; the screen is cleared to black by `0x080182A4(0)`. Vendor colours are `0x7304` for the selection, `0x31A7` for frames and `0xFFFF` for text.

**The patch:**
- **Added code:** `menu_list.c` is compiled to Thumb-2 with clang (`-Os`, freestanding). Its `.text` (364 bytes) is copied, unlinked, to `0x0807244C`.
  - The build refuses any object that needs relocations or has data sections.
  - Vendor routines are reached through absolute addresses.
- **Calls:** the three calls above are redirected with same-size `BL` changes:
  - both draw calls go to `lpm_menu_draw` at `0x0807244C`;
  - navigation goes to `lpm_menu_nav` at `0x08072560`.
- **Header:** the length grows to `0x685B8`.
- **Layout:** seven 38 px rows from y 44, with the selected row in `0x7304` and kept near the middle, and a scrollbar at x 235–237.
  - The status bar, header tab and footer stay vendor-drawn.
  - The partial-redraw argument is ignored, and each visible row is repainted.
- **Keys:** up/left select the previous item and down/right the next, wrapping at both ends. The selection byte keeps its vendor meaning, so `M` opens the same function.
- **No more garbage log lines:** the replaced draw never reads past item 10, so the `APP_GUI_Show_menu` lines are gone. `HOME: selete_fun:<item>` still logs the selection every 2 s.

**Offline check:** `.venv/bin/python scripts/preview_menu.py` runs the built image in the unicorn emulator, with the V2.0.7 SRAM snapshot `sram-20261019-204042.bin` supplying the label table.
- Only `fill_rect` and `draw_point` are replaced by Python, so the text uses the tester's own font.
- It writes one screen per selection to `output/menu-list/`.
- It also checks that ten presses of down visit every item and wrap, and that up wraps back.

## Update file size limit (found 2026-10-20)

**The update file may not be larger than the vendor file, 434,176 bytes (`0x6A000`).** This is inferred from one failed install and is not yet pinned to an exact number.

| Image | File size | Result |
| --- | --- | --- |
| `extended-hook` (payload `0x68478`) | 434,176 | Installed, rebooted by itself |
| `menu-list` (payload `0x685B8`) | 434,176 | Installed, rebooted by itself; the list menu works |
| First `home-ui` (payload `0x6CAA8`) | 438,272 | **Failed**; see below |

**What happened with the first `home-ui`** (`uart-20261020-014142-1000000.txt`):
- **No automatic reboot after the copy.** The tester rebooted only a few seconds after the owner deleted the file in Windows.
- **First boot:** `BootFlag 0x9718`, so the bootloader had run an install. The new background and the list dividers appeared, but **no text**.
- **The app then died.** At uptime 3846 ms (`Clear test end`) the log restarts at uptime 0 about 3.4 s later, with no `PWR_Off` line. That is a hang followed by a watchdog reset.
- **Later power-ons** stayed up only a few seconds with the same picture, then went off. A second install attempt of the same file at 01:50 behaved identically.
- **Reinstalling `menu-list` worked.** BOOT mode and the update path are unaffected by a crashing application.

**Cause, reproduced in the emulator:**
- Only the first 2,996 bytes after the vendor payload (up to flash `0x08073000`) were written. The rest stayed erased (`0xFF`).
- All of the code fitted below that point. The font structs and the icon tables lay beyond it.
- **Text silently vanished:** an erased font struct says every glyph is 255 px wide, so every glyph is clipped away. That is why the dividers drew and the text did not.
- **The fault:** the first icon read an erased pointer (`0xFFFFFFFF`), which is a bus fault. The vendor fault handler loops until the watchdog resets the chip.
- Truncating the image at `0x6A000` in the emulator produces exactly this: the dividers draw and `lpm_draw_icon` faults.

**Rules from this:**
- **`build_patched_image.py` now refuses** any image whose payload ends past `0x6A000`. The added code has **2,996 bytes** after `0x0807244C` (flash up to `0x08073000`).
- **Larger images are not worth trying again** until the limit is understood. Whether it is the bootloader or the USB drive that stops at that size is unknown.
- **Ways to get more room:** reuse the vendor code the redirects stranded, or find where the limit comes from. `python3 scripts/check_dead_code.py` reports **674 bytes** reusable once `home-ui` is applied:

| Range | Bytes | Was |
| --- | --- | --- |
| `0x0800F648`–`0x0800F800` | 440 | grid menu draw, replaced by `lpm_home_draw` |
| `0x080180C0`–`0x080181AA` | 234 | grid navigation, replaced by `lpm_home_key` |

**How the check works.** A range counts as dead only when nothing refers to it: no `BL`/`B.W`, no 16-bit branch, no stored Thumb pointer, and no PC-relative load from outside. Two rules keep it honest:
- **Branches are never filtered.** An earlier version discarded candidates that did not look like instruction boundaries, and that filter hid the vendor image's own calls into these ranges. Reporting a live range as dead is the dangerous direction, so a doubtful branch now counts as a reference.
- **A stored pointer counts only with bit 0 set**, because `BX` and `BLX` read that bit as the instruction set. Words without it, such as the encoding of `ldrb.w r0, [r0, #0x800]`, cannot be call targets.
- **It is validated both ways** by `tests/test_scripts.py`: the same ranges must look reachable in the vendor image and unreachable after the redirects, while `fill_rect` and `draw_string` stay reachable in both.

**The menu draw really ends at `0x0800F904`**, but the last 260 bytes are left out. A load at `0x0800F424` appears to read a literal at `0x0800F808`; that offset is provably the second half of the `push.w` at `0x0800F422`, but the range stops below it rather than depending on that judgement.

**This cannot see computed jumps** through a table or a pointer built at run time, so a pass is strong evidence, not proof. Keep a known-good image for recovery before installing anything that reuses these bytes.

## Chinese glyphs and where the spare bytes are (2026-10-20)

The owner noticed Chinese characters in `Settings -> About` and in the Factory Reset warning, and asked whether removing them would free space for the design fonts.

**The 16x16 glyph table at `0x080708F8` is the whole Chinese UI, not a few stray characters.** An earlier note here said the binary held no Chinese text and only two glyphs were drawn. Both were wrong: the scan treated strings as bytes, but they are arrays of glyph indices.
- **There is a language setting.** Cust_Info `+0xA5` is 1 for English and 2 for Chinese. `lang_is(n)` at `0x08010270` (39 callers) and inline checks of `+0xA5` choose between an ASCII label and a glyph-index string on almost every screen.
- **Chinese is the vendor default.** The defaults routine `0x0801B340` stores 2, and the boot load `0x08010288` forces 2 for any value other than 1 or 2. The toggles at `0x08011418` and `0x080117AC` switch between 1 and 2.
- **There are 67 call sites:** 35 to `0x08018F0C`, which draws a byte string of glyph indices ending at the first byte ≥ `0xAB`, and 28 to `0x08018C4C`, which draws halfwords: ASCII up to `0x7E`, a glyph when above `0xFF`. Most build their string on the stack from a literal pool.
- **Slots `0x00`–`0xAA` all hold real glyphs**, 171 × 32 = 5,472 bytes, ending at `0x08071E58`. Both text routines refuse indices ≥ `0xAB`. The bytes after that are other data, referenced from 22 places, and are not touched.

**What the owner saw is not in this table.** The company name in About (深圳市菲尼瑞斯科技有限公司) and the "⚠ 注意" header of the reset warning are pictures, drawn like the FNIRSI logo. With the glyph blitter disabled they still appear, in the emulator, exactly as before. They are removed separately (see "Chinese pictures").

**How the table is freed: English only.** Proving which of 171 glyphs 67 call sites can reach would have needed the strings followed through tables and arguments. Instead, the one reader is switched off, which needs only one proof.
- **`0x08018DB0` is the only code that forms an address in the table.** It does so at `0x08018DDA` and, for its second drawing mode, at `0x08018E78`. No stored pointer, literal, ADR or PC-relative computation anywhere else lands in the table. Both text routines and the two direct calls in the reset warning go through it.
- **`home-ui` patches its first instruction to `bx lr; nop`.** It takes no stack arguments and no caller uses its return value, so every caller still runs, and simply draws nothing for a glyph.
- **The language is held at English**, because in Chinese nearly every vendor label would now be blank. This was confirmed in the emulator.
  - The boot load (`0x0800FE78`, `0x0800FE82`) and the factory-reset defaults (`0x080106F4`) are redirected through wrappers in `src/firmware_patch/cust_info.c`, which set `+0xA5` to 1 after the vendor code. The boot wrapper returns the vendor result, which the caller retries on.
  - The defaults routine itself is not edited: it reuses `r0 = 2` for the Auto Off default.
  - The two "switch to Chinese" stores (`0x0801142A`, `0x080117D2`) store 1 instead.
  - The home screen checks again on every draw.
  - The boot load's own fallback to 2 (`0x080103A2`) sits in the protected flash-routine range, so it is left alone. The boot wrapper covers it.
- **The ASCII drawer could reach the lower part, but no caller lets it.** `0x08018A34` indexes its fonts with `(c - 0x20) & 0xFF` and no range check, so a character outside 0x20–0x7E would read up to `0x08071307`, inside the glyph table. At first only `0x08071308`–`0x08071E58` was reused for that reason. Link status needed more, so all 17 call sites were reviewed (2026-10-20):
  - `draw_string` (`0x08018D48`) and the halfword drawer (`0x08018C4C`) stop at any character outside 0x20–0x7E.
  - The number drawers (`0x08018CB4`, `0x08018F3E`) pass a space or `'0'` + digit.
  - Five pass a constant: `G`, `.`, `V`, `:`.
  - The rest draw a fixed count of characters from buffers formatted with `%03d.%03d.%03d.%03d` (the IP edit fields at `0x20000326`; every octet is 0–255, so always 15 characters) or `%03d%03d` (at least 6 characters). Only digits and dots.
  - So the **whole table, `0x080708F8`–`0x08071E58`, is reused.** `check_dead_code.py` records the 17 reviewed sites and fails if any other BL to the drawer appears, including from added code.

**Checked three ways:**
- **`scripts/check_dead_code.py`** now handles data as well as code.
  - **A data range is judged as part of its whole object.** The blitter forms the base `0x080708F8` and indexes upward, so a check limited to the reused addresses would have missed it. A test keeps that from regressing.
  - **It finds addresses computed from the PC:** ADR, ADR.W, and a literal added to PC, including a `subs` that follows.
  - **Each range can have a guard.** The table and the blitter body are dead only while `0x08018DB0` reads `bx lr; nop`, so the vendor image fails.
  - **The dead set is a fixpoint.** A reference from another dead range is ignored only while that range is dead itself.
  - Font bitmaps (`0x0806FE94`–`0x080708F8`) are marked as never executed, because their bits decode as stray branches.
  - **BLs the patches redirect into added code placed in a range are accepted.** The checker compares with the vendor file and ignores a branch at a site whose bytes the patches changed. Only branches qualify; pointers and PC-relative references never do.
- **Unit tests** check the patch sites, that the defaults routine is byte-for-byte the vendor's, that the table base is seen in the vendor image, that the reviewed caller list matches the image, and that an added call to the ASCII drawer is caught.
- **Emulator cross-check:** About (`0x08011A14`), the reset warning (`0x08011CC8`) and the six Settings rows (`0x08010734`, rows 0–5) were run with a memory-read hook on the reused range.

| Image | English | Chinese |
| --- | --- | --- |
| vendor | no reads | About 176, reset warning 192, Settings rows 80 reads, all from the blitter (`0x08018E8E`) |
| `home-ui` | no reads | no reads |

**Space now (home-ui, 2026-10-20, after reclaiming the unused pictures):**

| Region | Bytes | Used | Notes |
| --- | --- | --- | --- |
| After the vendor payload, `0x0807244C` | 2,996 | 1,718 | the routines the vendor calls into: home screen, drawing helpers, Cust_Info wrappers, the reset warning title |
| Pictures 9, 5, 22 and 23, `0x080584FC` | 89,512 | 13,348 | everything else: Link status (IPv4 and IPv6), fonts, icons, strings, tables |
| Pictures 4, 0, 2 / 8, 3 / 21, 7, 6 / 1 | 43,048 | 0 | cleared, not yet in the linker script |
| Glyph table, grid menu draw, grid navigation, blitter body | 6,498 | 0 | cleared, declared in the linker script, unused since the move |

**139,064 bytes are cleared for reuse** (`check_dead_code.py`), and about 119 KB of it is free. Any flash address is within BL range of any other, so code may sit in any of these as well as data.

**Checker notes:**
- **DEAD1 and DEAD2:** both were flagged as reachable by log strings that decode as a `ldr` into them: "%d GUI: APP_GUI_Show_String …" after `0x0800F4B4`, and "%d Charge by not PWR_ON %d" after `0x08017E74`. `0x0800F5D0`–`0x0800F608` and `0x08017F90`–`0x08017FD8` are now listed as data. The second range holds the pool words and two log strings of `0x08017E74`, reached by `adr` at `0x08017ED2` and `0x08017F64`.
- **Pictures:** all 24 pictures (`0x08026712`–`0x0806F2D9`) are listed as never-executed pixels.
- **Reviewed exceptions:** three matches are listed, each kept only while its bytes are unchanged.
  - The word at `0x0801EE10` is the instruction `ldrb.w r0, [r0, #0x805]`, which reads as `0x0805F890`.
  - The two apparent BLs at `0x08025C08` and `0x08025C4A` are the second halves of `lsr.w` instructions in the soft-float shift helper (decoded from its start, `0x08025B3C`).

## Pictures (Chinese ones removed 2026-10-20; unused ones reclaimed)

**How pictures are drawn.** `0x0800F422(x, y, index, mode)` asks the getter `0x0800E8E0` for a pointer and returns at once if it is NULL. A picture is an 8-byte header (width at +2, height at +4) followed by RGB565 pixels. The getter reads a 24-entry table at `0x200000F0`, in the RAM data the start-up code unpacks from flash `0x0807237C` (0x254 bytes to `0x20000000`, by `0x0800A1D4`). No code writes the table, and the getter's literal is the only reference to it. (An earlier note here said `0x0800E8AC` fills it; that routine posts message `0x3A`.) The pictures lie back to back from `0x08026712` to `0x0806F2D9`.

**The two Chinese pictures:**
- **Picture 21 (0x15), the reset warning title** (a warning sign and 注意, 70 × 21 at 78, 126, flash `0x0802FA09`). It is drawn by `0x0800F412`, which the warning `0x08011CC8` calls at `0x08011D3C`. That call now goes to `lpm_reset_warning_title` (`src/firmware_patch/vendor_text.c`), which draws "Warning" in the vendor 8x16 font in the picture's orange (`0xFCC0`). The warning sets its own colours again right after (`0x08011D40`).
- **Picture 22 (0x16), the company name in About** (深圳市菲尼瑞斯科技有限公司, 136 × 10 at 120, 211, flash `0x0805B359`). Its two calls (`0x08011B0C`, and `0x08011BCE` in the Chinese branch) are now `nop; nop`. About ends with the URL.

**Every way a picture is drawn**, and the indices each one can pass:

| Call | Index | Pictures | Status |
| --- | --- | --- | --- |
| `0x0800F8EE` (grid menu, `0x0800F648`) | menu item `r7`, 0–9 | grid icons 0–9 | dead: both calls redirected to `lpm_home_draw` |
| `0x0800FC1A` (GUI message `0x3B`) | from the message; only `0x0800F488` posts it | 23 from the QC draw `0x0800BB0C`, 20 from `0x0800E170` | 23 dead (QC draw is reached only from `0x0800FABE`, now `lpm_link_draw`); 20 kept |
| `0x0800F412` | 21 | warning title | dead: callers redirected or unreachable (`0x0800BBB8`, QC) |
| `0x08011B0C`, `0x08011BCE` | 22 | company name | removed |
| `0x0801589E` | 16 + a counter that cycles 0–3 | 16–19, a four-frame animation in state 5 | kept |
| `0x0800D514`, `0x0800D62A`, `0x0800EDB6`, `0x0800F61C`, `0x0800F624`, `0x08011A42` | 10, 11, 13, 12, 12, 15 | | kept |

Nothing holds a pointer to the getter, the drawer or `0x0800F488`, and only `0x0800F488` sends message `0x3B`. Entry 14 points at the one byte between pictures 3 and 15, and nothing asks for it.

**Reclaimed:** pictures 0–9, 21, 22 and 23, 138 KB in five blocks.
- **Getter patch:** their entries in the getter's `tbb` table (`0x0800E8EA`, one byte per index) now branch to `movs r0, #0` (byte `0x28`, i.e. `0x0800E93A`). Any path the review missed therefore draws nothing, rather than reading new code as a picture header.
- **Emulator test:** `tests/test_scripts.py` runs the vendor getter and drawer from the patched image. It checks that the freed indices return NULL and draw nothing, that the others return the table's pointers, and that picture 15 still draws. It also checks that no patch overlaps a kept picture.

**Other space looked at, not reclaimed:**
- **The vendor QC test code** (`0x0800BAE4` open, `0x0800BB0C` draw, `0x0800BD98` count init) is unreachable now. Its callers are redirected, and `0x08023314` sits behind the `bne → b` in the 1 s tick. It is only 1–2 KB.
- **Chinese text** is stored as two-byte glyph numbers for the CJK font, mixed with tables after the pictures. It is at most about 1.5 KB.
- **The grid menu tail** `0x0800F808`–`0x0800F904` (252 bytes) is left out for the reason given in `check_dead_code.py`.
- **UART log strings** are live, and they are how hardware tests are checked.

## Home screen in the working design: `home-ui`

`python3 scripts/build_patched_image.py home-ui` builds `firmware/patched/home-ui/LPM-10C_TX_V2.0.7_260907.bin`. Its SHA-256 is `efa2eff8…8035e6` (2026-10-20: the green arrow at the top-left port in Link status), confirmed working on the tester and kept in `firmware/patched/home-ui-known-good/` for backing out. The file is **434,176 bytes, the vendor size**. It draws the home screen of `output/screens/01-home.png` with the design's palette, icons and layout.

**What changed after the first attempt:**
- **Text uses the vendor fonts** (6×12 and 8×16) through `draw_string`, so no font data is added. The design's own 5×9 and 7×11 fonts are not used.
- **Icons are kept:** the nine 24×24 bitmaps come from `src/lpm_ui/lpm_assets.c` and are drawn through the LCD cursor and data register.
- **No RAM state.** The list window is derived from the vendor selection byte on each draw, so nothing reads uninitialised SRAM. It keeps the selection at the third row until the list ends.
- **Size:** 1,550 bytes of code and icons, payload ending at `0x08072C1C`.

**Build:**
- **Compile:** clang `-Os`, Cortex-M4, freestanding, `src/firmware_patch/home_ui.c` and `lpm_assets.c`.
- **Link:** `ld.lld` with `src/firmware_patch/home_ui.ld` at `0x0807244C`; `.data` and `.bss` are refused.
- **Display port:**
  - Rectangles go to the vendor `fill_rect` at `0x08018568`.
  - Text goes to `draw_string` at `0x08018D48`.
  - Icons go straight to the LCD with `lcd_set_cursor` (`0x080189E8`) and `lcd_write_start` (`0x08018FFC`), then pixels to `0x60020000`.

**Same call redirects as `menu-list`:**
- Both draw calls (`0x0800FA7E`, `0x0800FA9E`) go to `lpm_home_draw`, which redraws everything on each call.
- Navigation (`0x0800D802`) goes to `lpm_home_key`. The list stops at both ends, as the design specifies.
- The vendor selection byte is still written, so `M` opens the vendor function.

**Items, in design order** (the vendor function in brackets):
- Cable map (Cable Test)
- Cable length (LENGTH)
- Cable trace (SCAN)
- Port flash (FLASH)
- PoE voltage (POE)
- Link status (added, in the QC Test slot; see below)
- Link speed (SPEED; a speedometer icon, since Link status has the Ethernet port icon)
- Ping monitor (PING)
- IP scan (IP SCAN)
- Settings (SETTING)

**Not covered yet:** "Cable trace" for SCAN is assumed from the function's name.

**Status bar:** it shows `LPM-10C` only. The vendor routine at `0x0800EBE8` still draws its battery icon at x 176–215, y 4–20 after each full draw and periodically, and handles the low-battery warning, so it is left running.

**Battery icon flashing while scrolling (fixed 2026-10-20).** The first working build redrew the whole screen on every key press, including the status bar, but the vendor repaints the battery icon only after a **full** draw (`0x0800FA82`), not after the redraw on message 7 (`0x0800FA9E`). The icon was therefore erased on each key press and stayed blank until the next periodic refresh. `lpm_home_draw` now honours its `partial` argument: on a redraw it repaints only the list rows, the scroll bar and the `n/10` counter, and never touches the status bar, the title text or the footer.

**Fonts: Terminus (2026-10-20).** The screens draw with Terminus 6x12 and 8x16 (`src/fonts/terminus-4.49.1`, SIL Open Font License 1.1, copyright Dimitar Toshkov Zhekov; the licence is kept beside the fonts).
- **Why not the design fonts:** the design's 5x9 and 7x11 rendered unevenly on the tester's screen. The owner compared four fonts in the emulator (`output/font-compare/`: design, the vendor's 6x12/8x16, Spleen and Terminus) and chose Terminus.
- **`scripts/gen_subset_font.py` emits only the characters that are drawn**, read from the string literals in the sources that draw with this font (`home_ui.c`, `link_status.c`, `draw.c`) plus a fixed set of digits and separators that screens build at run time. `menu_list.c` and `vendor_text.c` draw with the vendor font, so their text needs no glyphs here. That is **62 characters, 1,831 bytes** including a 95-byte index. A character with no glyph is drawn blank, never as a wrong glyph, and `tests/test_scripts.py` fails if the generated file is stale. `--small`/`--body` take another `.bdf`, `design` or `vendor`.
- **Baseline:** a font taller than the design's is drawn with its baseline where the design font's was (`LPM_SUB_*_DY`, −3 rows for both Terminus sizes), so the screen layouts did not change.
- **Blitting** is one LCD window per row of the whole string rather than per glyph, so the cursor is set once per row and pixels stream out.
- **Where it sits:** `home_ui.ld` puts the glyphs, index, icons and strings with the rest of the added data in the reclaimed pictures (see "Pictures"). `build_patched_image.py` refuses any linked section that falls outside a range `check_dead_code.py` has cleared, and a test checks the two scripts agree on those ranges.
- **DEAD1 check:** `check_dead_code.py` had flagged DEAD1 as reachable because the log string after `0x0800F4B4` ("%d GUI: APP_GUI_Show_String …", `0x0800F5D0`–`0x0800F608`) decodes as a `ldr` into it. That range is now listed as data; no branch reaches it.

**Offline check:** `scripts/preview_menu.py` now runs the **vendor drawing code unmodified**, with only the LCD controller modelled on the bus (commands at `0x60000000`, data at `0x60020000`, command bytes read from the config at `0x20002FCC`). The earlier version replaced the vendor routines with Python shortcuts, so it could not have shown this failure.
- It reports a call that does not return as a hang, an invalid access or instruction as a fault, and the deepest stack use (168 bytes here).
- It steps through all ten items; each redraw must equal a full redraw pixel for pixel.
- It does **not** model the flash size limit. Truncate the image to `0x6A000` by hand to test a candidate.

## Link status (added 2026-10-20; confirmed on the tester with IPv6, scrolling and the port arrow)

A new test, described by the owner: it checks the physical link, gets an address by DHCP, checks the gateway with ARP and pings it, showing PASS or FAIL for each step. It follows the Checklist tab of `output/screens/11-link-checklist.png`. The code is in `src/firmware_patch/link_status.c`, and the shared drawing helpers are in `draw.c`.

**Where it lives: the vendor QC Test slot** (menu index 4, system state 8). QC Test runs the cable-count check against the calibration in Cust_Info `+0x90`. It was the one tool with no design screen. Link speed (SPEED) is kept, because it uses the gigabit YT8521SC, which Link status cannot see.

**How the vendor UI is driven** (all found 2026-10-20):
- **Keys become events through a 43-entry table** at `0x08026264`, `{state mask, key, press type, event}`, matched against `1 << state` by `0x08016180` in the key task. Keys: 0 BACK, 1 right, 2 down, 3 up, 4 M, 5 left; press type 3 is a short press. In state 8 the vendor table has only BACK (event 7, back to home) and a held right key (event `0x0B`, the QC count init).
- **The event dispatcher** `0x0800D794` (`tbb`, events 0–`0x2A`). Event 6 opens the selected tool by `0x20000150[1]`; for QC Test it calls `0x0800BAE4`. Event `0x0B` calls `0x0800BD98(0)`, a command to the QC count task. It is reached only from state 8.
- **The GUI task** draws on messages: `0x36` (posted by `set_sysState` for tool states) calls the tool frame `0x0800EE8C`, which clears the screen and draws the vendor frame, title and battery; `0x0C` calls the QC draw `0x0800BB0C`.
- **The 1 s tick** (`0x080232C8`) sends the QC count task a measure command in state 8 (`0x0802330C`). It sends the app task network work (`cmd 4`) only in PING and IP SCAN, and only while the busy flag `0x200000A9` is clear. Command 6 ends in `bx lr`.
- **Blocking network work runs in the key task.** The vendor Auto Config IP (`0x0801CA64`) does exactly this from the PING key handler, with `0x200000A9` set. The watchdog (IWDG, prescaler /32) is fed from a timer interrupt (`0x08019DEC`) and the UART task, not from the key task, so a run of several seconds is safe.

**Patches:**

| Site | Was | Now |
| --- | --- | --- |
| `0x0800D830` (event 6) | QC open `0x0800BAE4` | `lpm_link_open`: clear the state, `set_sysState(8)`, run |
| `0x0800D8CC` (event `0x0B`) | QC count init `0x0800BD98` | `lpm_link_run` |
| `0x0800FABE` (message `0x0C`) | QC draw `0x0800BB0C` | `lpm_link_draw`: the rows that fit from the top one, and the scroll bar |
| `0x0800FC20` (message `0x36`) | tool frame `0x0800EE8C` | `lpm_tool_frame`: in state 8 the whole screen in the design, then the vendor battery icon; otherwise the vendor frame |
| key table entry 19 (`0x08026300`) | key 1, held | key 4 (M), short press |
| `0x0802330C` | `bne` | `b`: no QC count command from the tick |
| key table entries 2 and 3 (`0x08026274`, `0x0802627C`) | UP, DOWN in state 2 (mask `0x004`) | also state 8 (mask `0x104`): events 2 and 3 reach `lpm_home_key`, which passes them to `lpm_link_key` in state 8 |
| key table entries 4 and 5 (`0x08026284`, `0x0802628C`) | LEFT, RIGHT short presses in state 2 | also state 8: events 4 and 5 change the tab (added 2026-10-21) |

So `M` on the home item opens the screen and runs it once, `M` runs it again, `UP` and `DOWN` scroll, and `BACK` returns home (the vendor's own event 7). The home screen's UP and DOWN entries were the only ones to widen: they come before any state-8 entry, and no other entry gives state 8 those keys (a test checks this). The QC long-press "init count" is no longer reachable, which also protects the count calibration from an accidental overwrite.

**The checks**, each drawn as soon as it finishes (message `0x0C`). A failed check skips the rest, except that the last three all run once the gateway is found: many routers ignore ping but still route and resolve.
1. **Physical link:** DM9051 register 1 bit 6, waiting up to 3 s (`0x08022088`) if it is down. Speed is register 1 bit 7 (1 = 10 Mb/s); duplex is register 0 bit 3.
2. **Address:** DISCOVER and OFFER, two tries of 1.5 s, then REQUEST and ACK, with the vendor's own DHCP routines (`0x0801FE38` send, `0x0801FFD4` wait) and their timing. A lease is PASS. An OFFER with no ACK is WARN ("OFFER only, no lease"), where the vendor Auto Config IP would silently use the offered address. The ARP and ping steps then use the offered address.
3. **Gateway ARP:** the vendor request and wait (`0x0802144C`, `0x08021FEC`), two tries of 1 s. The gateway MAC is read from the reply left in the frame buffer. With no gateway in the lease it is FAIL, "No gateway known".
4. **Gateway ping:** the vendor ping is a state machine shared with PING and IP SCAN, so the echo is built here (IPv4 + ICMP, 32 bytes, RFC 1071 checksums) and sent with the vendor `transmit` (`0x080228B8`). Up to three requests, 1 s each. The receive loop answers an ARP request for our address, in case the gateway has not cached us. The round trip is timed with the DWT cycle counter (144 MHz, from the SysTick reload of 143,999). Polling does not sleep for the first 20 ms, so a fast reply is timed in microseconds.
5. **Internet ping:** the same echo to 1.1.1.1 and, only if it does not answer, to 8.8.8.8, two requests of 1 s each, addressed to the gateway's MAC. PASS shows which address answered and its round trip. This proves routing to the Internet; name resolution is step 6.
6. **DNS lookup:** a query for `example.com` (type A, recursion desired) to the first DNS server the DHCP reply names, then, only if that fails, to 1.1.1.1. Two queries of 1 s each per server.
   - **Where the server comes from:** the vendor `dhcp_wait` (`0x0801FFD4`) reads only options 1, 3, 53 and 54, and returns as soon as the reply matches, leaving the whole reply in the frame buffer (`0x2000F0AC`, length at `0x20000224`). `dhcp_dns` reads option 6 from it after the OFFER and again after the ACK.
   - **The packet:** built here as UDP from port 50000 to 53, with the UDP checksum left 0 (allowed over IPv4). A server on our subnet other than the gateway (a local resolver) is found by ARP first; any other is reached through the gateway.
   - **An answer counts** when it is a response to our id from that server, with RCODE 0 and at least one answer record. SERVFAIL, NXDOMAIN or an empty answer counts as no answer.
   - **Verdict:** PASS when the DHCP server answers; WARN when only 1.1.1.1 answers ("DHCP DNS failed" or "No DHCP DNS"), because clients on that network would have no working DNS; FAIL when neither does. The detail shows the name and the address in the answer (`example.com = 93.184.215.14`), then the server that answered, where it came from, and its round trip. The address is the first A record in the answer section, after skipping the question and any CNAME records (names may be compressed); a response without one counts as no answer.
   - **Worst case,** with nothing answering after the gateway, the whole run takes about 8 s.

**IPv6 checks** (rows 7 to 11, under an IPv6 heading), run whenever the link is up, whatever the IPv4 checks found:
- **The receive filter.** The vendor DM9051 setup (`0x080225D2`) fills the multicast hash with only bit 63 (broadcast) and sets receive control (register 5) to `0x31`. Router advertisements go to `ff02::1` and a router's neighbour solicitations to our solicited-node group, both multicast, so the IPv6 checks set bit 3 (pass all multicast) with `dm9051_write` (`0x08016AE0`, found as the routine the vendor receive uses to reset register 5) and restore the value read before. The vendor `receive` rewrites `0x31` after a receive error; the wait loop sets the bit again when that happens.
- **Router:** our link-local address is `fe80::` plus the modified EUI-64 of the MAC. A duplicate address probe (a neighbour solicitation from `::`) goes out first, then up to four router solicitations from it, 1 s apart (a router may delay a multicast answer by up to 3 s, RFC 4861), carrying our MAC. PASS for an advertisement with a router lifetime; WARN when the only ones have lifetime 0 (not a default router). The detail is the router's address and the first prefix (an autonomous /64 if there is one). The router's MAC comes from its link-layer address option, else the Ethernet source.
- **SLAAC:** the autonomous /64 prefix plus our interface identifier, probed for duplicates. NONE when the router offers no such prefix.
- **DHCPv6:** a SOLICIT to `ff02::1:2` (DUID-LL client identifier, IA_NA with IAID 1, rapid commit), two tries; a REPLY leases at once, an ADVERTISE is followed by a REQUEST with the server's DUID. PASS with a lease, WARN with an ADVERTISE only or a server that refuses while the router sets M, FAIL with no reply when M is set, NONE with no reply when it is not (most home networks).
- **Gateway ping:** ICMPv6 echo to the router's link-local address, as the IPv4 one.
- **Internet ping:** to `2606:4700:4700::1111`, else `2001:4860:4860::8888`, from the SLAAC address (else the DHCPv6 one), two each. Skipped without a default router or a global address.
- **Neighbours:** while waiting, a neighbour solicitation for one of our addresses is answered with an advertisement (the router asks before it forwards an echo reply to a new address). A host that answers our probe, or probes the same address, marks it in use (FAIL, "In use by another host (DAD)"). Frames from our own MAC are ignored, in case the chip ever loops one back.
- **Checksums** are computed over the IPv6 pseudo-header (RFC 8200) for ICMPv6 and UDP alike; neighbour discovery goes out with hop limit 255.

**The port arrow** (added 2026-10-20). Vendor tools mark the port to plug into with a green arrow: GUI message `0x37` calls `0x0801B21C`, which switches on `0x0800E408(index, on)` (`APP_GUI_Draw_Note_Mark` in its UART log) per state. The arrows: 0 and 1 on the right edge (y 70–102 and 186–218), 2, 3 and 4 along the top (x 3–19, 112–128, 220–236). Cable Test and QC Test mark 0, FLASH, Length, SPEED and POE mark 1, PING and IP SCAN mark 2, SCAN marks 4. "Off" repaints an arrow in the vendor panel grey `0x31A7`. The vendor QC draw posted `0x37` itself (`0x0800BB80`), so Link status had no arrow. `lpm_tool_frame` now calls `0x0800E408(2, 1)` directly, marking the top-left port (the DM9051 port PING uses), and not the state-8 case, which would mark arrow 0 and paint arrows 1 and 0 grey over the scroll bar. The status text "LPM-10C" starts at x 24 on this screen, clear of the arrow. The emulator runs the arrow routine (with the FreeRTOS critical section `0x0802443C`/`0x08024460` and the log queue send `0x08024828` stubbed) and checks the arrow's pixels.

**The list scrolls.** The twelve rows (six IPv4 checks, the heading, five IPv6 checks) are taller than the screen. The first screen is exactly the IPv4 checks; `DOWN` moves a row at a time to the last screen, which ends with the IPv6 Internet ping. While the checks run, the list follows the one running, and it ends scrolled to the bottom. Rows take one to three detail lines, and long lines break after a space, colon or comma, so a full IPv6 address fits. After a scroll the view is cleared first, as rows differ in height; a step finishing redraws in place, without a clear, so there is no flicker.

**Nothing is saved.** The vendor Auto Config IP copies its result into the device IP settings (`0x20000306`) and Cust_Info. Link status keeps its results in its own state only.

**RAM.** The state (200 bytes; a `_Static_assert` pins the layout the emulator reads) is at `0x20020000`, above the vendor's RAM, which ends at the initial stack top `0x20011298`. No vendor code addresses anything above it. Both SRAM dumps show that region holding the chip's power-up pattern (the same random bytes with a few flipped bits). The state is never assumed to be zero: `lpm_link_open` clears it before `set_sysState` posts the frame message.

**Offline check:** `scripts/preview_link_status.py` runs the added code unmodified in the emulator against a model of the vendor network routines. It renders 29 scenarios to `output/link-status/`, each as the top of the list (`<name>.png`) and its end (`<name>-end.png`). The IPv4 ones: all pass, the gateway asking for our MAC first, no link, 10 Mb/s half duplex, no DHCP, OFFER only, no gateway, no ARP reply, the gateway ignoring ping, only 8.8.8.8 answering, no Internet, nothing answering ping, the DHCP DNS server returning SERVFAIL, no DNS server in the DHCP reply (with and without 1.1.1.1 answering), a local DNS server found by ARP, a DHCP DNS server of 1.1.1.1 that does not answer, and an answer that starts with a CNAME. The IPv6 ones: no router, a DHCPv6 lease (ADVERTISE then REPLY), rapid commit, ADVERTISE only, a server with no address, no server although M is set, DHCPv6 without SLAAC, router lifetime 0, a SLAAC address another host defends, only Google answering, and IPv6 with no IPv4 DHCP.
- **The model:** the gateway rejects an echo request whose IP or ICMP checksum is wrong, and 1.1.1.1 and 8.8.8.8 answer through it after 12 ms. Each DHCP reply is left in the frame buffer as a full BOOTP frame whose options (with pad bytes) may name DNS servers. DNS queries are checked for headers, ports and the exact question, then answered with an address, an error code, or nothing.
- **A deliberately broken build is caught:** with the RCODE check removed, the SERVFAIL scenario fails.

Each scenario checks:
- the six verdicts and the detail text;
- the three RTTs;
- that 8.8.8.8 is pinged only when 1.1.1.1 does not answer, and both twice when neither does;
- the DNS server read from DHCP, and which servers were queried, in order;
- that the device IP settings and Cust_Info are untouched;
- that the busy flag is cleared;
- that only messages `0x36` and `0x0C` are posted.

IPv6 in the model: the router answers each solicitation with a multicast advertisement, and asks for our MAC (a multicast neighbour solicitation) before it forwards an echo reply to our global address; multicast frames reach `receive` only while register 5 passes all multicast. Each scenario also checks that register 5 is set and then restored to `0x31`, the duplicate address probes, the number of router solicitations, the DHCPv6 messages sent, every IPv6 checksum, hop limit and Ethernet destination, and that only 2606:4700:4700::1111 is pinged when it answers.

Scrolling: each scenario presses `DOWN` twelve times and `UP` twelve times through `lpm_home_key`, redrawing after each, and checks that the list moves one row at a time, stops at both ends, ends the run at the bottom, leaves the menu selection alone and posts only message `0x0C`.

The deepest stack use is 400 bytes.

### Switch port tab (added 2026-10-21; emulator only, not yet on the tester)

`RIGHT` in Link status shows a second tab, **Switch port** (`src/firmware_patch/switch_port.c`); `LEFT` returns to the checklist. `M` on it listens on the DM9051 port for what the switch says about the port the tester is plugged into:

| Row | Source |
| --- | --- |
| Physical link | DM9051 registers, as the checklist |
| Switch | LLDP system name (else chassis ID) and description, or CDP device ID and platform |
| Port | LLDP port ID and description, or CDP port ID |
| VLAN | LLDP 802.1 port VLAN ID and LLDP-MED voice VLAN (network policy, application 1), or CDP native and voice VLAN; plus every 802.1Q tag seen on received frames |
| Management | LLDP management address (IPv4), or the first CDP address |
| PoE | 802.3 power via MDI (class, and the allocated power of the 802.3at extension), LLDP-MED extended power, or CDP power available |
| 802.1X | An EAPOL-Start to `01:80:C2:00:00:03`, repeated every 5 s; an EAP Request/Identity in answer is WARN ("Required"), none is NONE |

- **Asking instead of waiting.** LLDP switches send every 30 s and CDP every 60 s. The run first sends one LLDP frame announcing an LLDP-MED class I endpoint (chassis and port ID by MAC, TTL 120). An LLDP-MED switch answers that at once (fast start, ANSI/TIA-1057), so a typical managed switch is heard within a second. The run then waits up to 32 s, and stops 3 s after the first LLDP or CDP frame, which leaves time for the 802.1X answer.
- **The filter.** LLDP, CDP and EAPOL go to multicast addresses, so receive control bit 3 (pass all multicast) is set for the run and the value read before is restored, as for IPv6.
- **Parsing.** Every TLV length is bounds-checked against the frame. Text is cut to 31 characters and non-printable bytes become `?`. An ID that is six non-printable bytes is shown as a MAC address. When a switch sends both, LLDP's name and port win over CDP's.
- **Rows** that the switch did not announce are NONE; a missing link makes the rest SKIPPED. The countdown in the Switch row is redrawn once a second.
- **RAM.** The state (168 bytes, its layout pinned by a `_Static_assert`) is at `0x20020100`, after the checklist's. `lpm_link_open` clears it, so Link status always opens on the checklist.

The emulator (`preview_link_status.py`) has four more scenarios. In each, Link status is opened with the cable unplugged, then the cable is plugged in, `RIGHT` is pressed, then `M`:
- an LLDP-MED switch (answering the fast start, with tagged frames, PoE class 4 at 25.5 W and a 64-character description);
- a CDP-only switch that answers at 9 s on an 802.1X port;
- a silent switch;
- no link.

Each scenario checks the rows and details, the LLDP and EAPOL frames sent, the listening time, the number of EAPOL-Starts, and the receive control being restored. It also checks that `LEFT` brings the checklist back.

**Not done yet:**
- The design's DHCP tab (the DISCOVER/OFFER/REQUEST/ACK timeline).
- CDP is only heard when the switch's 60 s timer falls inside the 32 s window, since CDP has no request.

## Ping monitor (added 2026-10-21; emulator only, not yet on the tester)

A continuous ping with a latency graph, after the design's `output/screens/03-ping.png`, in the slot of the vendor PING tool (menu index 7, system state 11). The code is in `src/firmware_patch/ping_monitor.c`. Holding M opens an editor for the target address; the vendor PING screen is no longer reachable.

**What it shows:**
- the latest round trip in the design's large numerals, or TIMEOUT;
- the loss so far ("6 of 150", "4.0% lost");
- a graph of the last 190 round trips, one per pixel column, on a 1-2-5 scale that fits the highest one shown, with a red mark at the top for each lost ping;
- minimum, average, maximum and variation (the mean change between consecutive replies) over the whole run;
- the target and the sequence number.

**Starting and stopping.** It opens waiting ("Press M to start"). M starts a run: a fresh DHCP address and the gateway's MAC with the first three Link status checks (`lpm_net_prepare` in `link_status.c`, `net.h`; never saved), then one echo a second. M again stops it and keeps the results on screen. A run also stops when an echo goes unanswered and the DM9051 reports no link: the reading shows NO LINK, so pulling the cable does not pile up timeouts. LEFT and RIGHT cycle the target: the gateway, 1.1.1.1, 8.8.8.8, and the vendor PING tool's saved target (`0x20000306 + 0x14`) when one is set. During a run that restarts the statistics; when stopped it clears the old target's results. A target on our subnet is found by ARP; any other goes through the gateway.

**One echo a second without blocking keys.** Like the Link status checks, the pings run in the key task, which the vendor feeds from a FreeRTOS queue at `0x20000054` (`xQueueReceive` `0x08024940` with no timeout, then the key matcher `0x08016180`, whose items are `{key, press}`). While it waits for a reply or for the next second, the monitor polls that queue every 10 ms. Each key goes through the vendor key table as usual, so:
- BACK (event 7) leaves at once;
- M, LEFT, RIGHT and M held (events `0x29`, `0x27`, `0x28`, `0x2A`) arrive at `lpm_ping_key`, which only sets a request while a run is going;
- a ping interrupted by a key is not counted.

The round trip is timed with the DWT cycle counter, as in Link status.

**The busy flag drops keys (found on the tester, 2026-10-22).** The first build kept the network busy flag `0x200000A9` set while the screen was open, and on the tester no key worked: not BACK, not M. Without a cable it sat on the "No link" screen until Auto Off switched it off. The vendor key scan (`0x0801630E`) returns without queueing the key while the flag is set, so the vendor uses it to lock the keys during its blocking network work. Link status sets it only during a run, which reads no keys, so it never showed there. The emulator missed it because it put keys straight into the queue. Its key model now drops presses while the flag is set, and every Ping monitor scenario checks that none was dropped.

**Keeping the vendor PING engine away, without the busy flag.** In states 11 and 12 the 1 ms tick (`0x080233C0`) sends the app task command 4 (`0x08015114(4)` at `0x080233E6`) whenever `0x20000095` and the busy flag are clear. Command 4 is the vendor PING and IP SCAN network work (`0x0801D25C`: it sets `0x20000095` and pings or scans with the settings at `0x20000306`). That call now goes through `lpm_ping_tick_work`, which drops it in state 11 and passes it on for IP SCAN (state 12). The other periodic command, 6 every 2 s, ends in `bx lr` (`0x0801A676`).

**Auto Off.** `0x0800FEAC` (each second, outside home) counts idle seconds in `0x2000016C` against the Auto Off setting (Cust_Info `+0xA2`). The key matcher clears the count after every key (`0x0800FF04`, called at `0x08016252`). A run clears it after each ping, so a long unattended run is not switched off; once stopped, Auto Off works as usual. (`0x08011C80`, called before each key event, is the key click: it starts the buzzer PWM at `0x4000043C`.)

| Site | Was | Now |
| --- | --- | --- |
| `0x0800D84E` (event 6, menu index 7) | PING open `0x08013794` | `lpm_ping_open`: clear the state, set the PING screen byte `+0x1E` to 0, `set_sysState(11)`, run |
| `0x0800D890` (event 7, state 11) | PING back `0x080137B8` | `lpm_ping_back`: the vendor back, which goes home from screen 0; a run sees the state change and ends |
| `0x0800D984` (events `0x25`–`0x2A`) | PING / IP SCAN keys `0x08014778` | `lpm_ping_key`: the monitor's and the editor's keys; IP SCAN passes through |
| `0x0800FB04` (GUI message `0x15`) | `0x0801378E` (`bx lr`) | `lpm_ping_draw`: the readings, graph, target and footer |
| `0x080233E6` (1 ms tick, states 11 and 12) | app command `0x08015114(4)` | `lpm_ping_tick_work`: dropped in state 11 |
| `lpm_tool_frame` (message `0x36`) | | `lpm_ping_frame` in state 11: the labels, footer and port arrow |

**The target address editor.** The first build handed over to the vendor PING screen on a held M, for its target setting; on the tester that meant leaving the monitor and its look. Holding M now opens an editor in the monitor itself (stopping a run first): the address as twelve digits, UP/DOWN change the digit under the cursor (a hundreds digit 0–2, an octet at most 255), LEFT/RIGHT move it, M uses the address and BACK keeps the old one. The address goes to the vendor PING target (`0x20000306 + 0x14`). That is RAM outside the Cust_Info record, so it lasts until power off and is never written to flash. It becomes the selected target, and 0.0.0.0 clears it. The editor starts from the target shown, or the saved one before the gateway is known. The vendor's own static address for the tester (Set IP) is no longer reachable; the monitor always takes a DHCP address.

**RAM.** The state (832 bytes, its layout pinned by a `_Static_assert`) is at `0x20020200`, after Link status's and Switch port's. Statistics use 64-bit sums, divided by a shift-and-subtract routine because the build links no runtime library.

**Offline check:** `preview_link_status.py` has ten Ping monitor scenarios. Key presses go through the real vendor key table and dispatcher in the emulator, both from the key task and from a run's own polling, and are lost while the busy flag is set, as on the tester:
- opening and BACK without M: nothing is pinged;
- 150 pings to the gateway with spikes and five losses: the ring, the loss, the minimum, maximum and mean, the 1 s spacing, and the Auto Off count cleared after each ping;
- M to stop, then RIGHT while stopped: four echoes only, and the old results cleared;
- RIGHT to 1.1.1.1 during a run: the echo targets;
- LEFT with no saved target: it skips to 8.8.8.8;
- no link: the message, then BACK goes home;
- the cable pulled during a run: it stops with NO LINK;
- M held during a run to 1.1.1.1: it stops, LEFT and UP make 1.1.1.2, M uses it, and the next run pings it;
- the editor left open: four LEFTs and two DOWNs on 192.168.1.1 give 192.168.9.1 (digit wrap), and its screen;
- BACK in the editor: the saved target is unchanged, and a second BACK goes home.

A separate check calls `lpm_ping_tick_work` in states 11 and 12.

## Theme (added 2026-10-21; emulator only, not yet on the tester)

The vendor tools now use the home screen's navy theme and Terminus font, while their layouts and logic stay unchanged (`src/firmware_patch/theme.c`).

**Every pixel goes through five routines.** Rendering all 161 vendor screens in the emulator (`scripts/preview_vendor.py`) and following the LCD writes shows that every one passes through five small routines, so their entries now branch (`b.w`) to replacements:

| Vendor routine | Replacement | Used by |
| --- | --- | --- |
| `0x080182A4` clear(colour) | `lpm_theme_clear` | screen clears |
| `0x08018450` draw_point(x, y), colour at `0x200001A0` | `lpm_theme_point` | lines, frame polylines (`0x080184B4`), the picture blitter (`0x080183EC`) |
| `0x08018568` fill_rect(x0, y0, x1, y1, colour) | `lpm_theme_fill_rect` | boxes, panels, bars |
| `0x080188A4` plot(x, y, colour), at y + 40 | `lpm_theme_plot` | the vendor chart plotter |
| `0x08018A34` draw_char(x, y, c, size, mode) | `lpm_theme_char` | `draw_string` `0x08018D48` and the number drawers; size 12 is 6×12, anything else 8×16; mode 0 opaque, 1 transparent |

**Colours are mapped by value.** The 17 vendor colours measured over those screens map to theme colours:

| Vendor | Theme |
| --- | --- |
| `0x0000` background | BG |
| `0xFFFF` text | FG |
| `0x2105` frame body | PANEL |
| `0x31A7` input boxes and frame edges | FIELD |
| `0x4A69` label boxes and disabled text | RAISED |
| `0x7304` selection fill | ACCENT_DIM |
| `0xFE60` selection outline and text | ACCENT |
| the blues | ACCENT |
| the yellows and orange | WARN |
| the greens | PASS |
| the reds | FAULT |

Anti-aliased picture pixels that are near-grey are moved onto the background-to-foreground ramp, and selection yellows become the accent at the same brightness. The theme's own colours pass unchanged (the added screens also call `fill_rect`), and a test checks that no vendor key is itself a theme colour.

**Text.** Every character goes through `draw_char`, so the vendor screens draw in Terminus too. The font now keeps all 95 printable ASCII characters.

**Titles.** The tool frame's title (`draw_string(13, 21, 88, 20, 16, label)` at `0x0800F3F8`) is redirected to `lpm_tool_title`, which draws the name from the home list ("Ping monitor", not "PING") in a 120-pixel field.

The vendor frame geometry is kept: a full home-style header would overlap content that starts near y 45.

## Settings record (V2.0.3, RAM copy)

```
0x00-0x8F  all zero
0x90       eight LE half-words, one per conductor     (the cable-count init data)
0xA0       9718 01 05 05 01 0000 ...                  (magic 0x9718, then bytes 01 05 05 01)
0xA8-0xC7  IP settings and other fields               (values from the owner's unit not published)
```

- **`+0x90`, 16 bytes: the cable-count init data.** These are eight per-conductor values of similar size (about 220 on the unit studied). They match the UART line `CNT: APP_CNT_InitData_Init 0 <= <value>, size:16`, and V2.0.7 passes `+0x90` to the count module. This is the closest thing to a calibration found. It can probably be regenerated through the tester's own init procedure (`CNT: Test result error!!! Please init again.`), but treat this copy as the reference.
- **`+0xA0`: the validity magic `0x9718`.**
- **`+0xA5`: language**, 1 English, 2 Chinese (the vendor default). V2.0.7 forces it to 2 unless it is 1 or 2. It holds 1 here. `home-ui` keeps it at 1 (see "Chinese glyphs").
- **The other fields** (volume, units and so on) are not decoded.
- **The firmware rewrites this record to flash at every power-off.** UART `uart-20261019-195246-1000000.txt` shows `PWR_Off by reason 1` followed by `APP_Home_Cust_Info_Storage Success`. A RAM snapshot taken just before power-off therefore matches what is stored.
- **Restoring it** would need firmware that writes it back. Nothing in the vendor update path restores it. If an update resets settings, compare the new record with this one.

### After the update to V2.0.7 (SRAM dump `sram-20261019-204042`)

- **The record is at `0x20002CF0`**, the RAM address static analysis predicted for V2.0.7. An identical copy is at `0x2000A198`.
- **A third hit at `0x20016C8C` is a false positive:** random-looking bytes that happen to contain `0x9718` at `+0xA0`.
- **Compared with the V2.0.3 backup, exactly one byte differs: `+0xA2` went from 1 to 2.** Everything else is unchanged, including the cable-count values at `+0x90`.
- **What `+0xA2` is:** V2.0.7 treats it as a setting in the range 0–3, default 2 (`0x0801B340`). It is stepped with the left/right keys on the settings screen (`0x080117F4`–`0x0801182A`).
- **`+0xA2` is Auto Off.** The option strings sit in order at file `0x7B50`: `OFF`, `5min`, `10min`, `15min`, which are indices 0–3. The label `Auto Off` is at `0x69338`.
  - V2.0.3 stored 1 (5 min) and V2.0.7 shows 2 (10 min, the default).
  - The owner had set 5 min before the update and saw 10 min afterwards (owner report, 2026-10-20).
  - So the update reset Auto Off to its default. This is not something the owner did in SETTING (UART, 20:28:43–47).
  - The code path that resets it is not traced. 1 is inside the valid range, so the 0–3 range check alone does not explain the reset.

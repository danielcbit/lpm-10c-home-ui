# Contributing

Thanks for helping. This project changes the firmware of a device people own and
cannot easily repair, so the rules below are mostly about not bricking testers
and not publishing things that are not ours to publish.

## Ground rules

- **Never commit FNIRSI firmware.** No vendor `.zip` or `.bin`, no extracted or
  patched images, no disassembly listings of vendor code longer than the few
  instructions a patch note needs. `.gitignore` keeps `firmware/` and `*.bin`
  out; do not force-add them. Releases publish a byte patch, never an image.
- **Never commit captures from your unit.** UART logs, SWD dumps, settings
  records and photos carry your MAC address, network settings and calibration.
  Keep them under `evidence/` (ignored). The emulator runs from
  `tests/fixtures/sram-sanitized.bin`; see its README before changing it.
- **Read-only tools stay read-only.** The UART helper opens the port receive
  only. The SWD Tcl scripts must not halt, reset, write or erase, and
  `tests/test_scripts.py` checks this. Never add an unlock, mass-erase or
  "recover" step: on the N32G455, going from read protection level 1 to 0 erases
  the flash, including the factory calibration.
- **The update file must stay 434,176 bytes.** The bootloader installs larger
  files only in part and the tester then crashes at boot. `build_patched_image.py`
  refuses to write a bigger image; keep it that way.
- **Do not write flash more than the vendor did.** The settings page is written
  only when a setting changed (see the flash wear section of the README). New
  code must not add writes to flash.

## Setup

macOS (Homebrew) is what the author uses; CI uses Ubuntu 24.04 with the LLVM
release tarball.

```sh
brew install llvm@22 lld python
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4 capstone==5.0.7
```

| Tool | Version | Used for |
| --- | --- | --- |
| clang with the ARM target | LLVM 22 (`/opt/homebrew/opt/llvm@22`, or set `CLANG`) | firmware patch code, `make arm-size` |
| ld.lld | 22 or later (Homebrew `lld` 23.1.3) | linking the added code after the vendor payload |
| Host C compiler | any C99 compiler with ASan/UBSan | `make test` |
| Python | 3.8 or later (3.12 in CI, 3.14 locally) | scripts and tests |
| unicorn | 2.1.4 | emulator previews and the Link status scenarios |
| capstone | 5.0.7 | inspection scripts, dead-code check |

Put the official FNIRSI LPM-10C V2.0.7 update where the build expects it. The
command checks its SHA-256 first:

```sh
curl -fLO https://www.fnirsi.com.cn/uploads/file1/20261010/d68467f735b832bc57860df7f230ada41791625743186.zip
python3 scripts/lpm_patch.py extract d68467f735b832bc57860df7f230ada41791625743186.zip
```

Without it, the firmware tests are skipped and only the host UI and script tests
run.

## Workflow

```sh
make test PYTHON=.venv/bin/python             # everything: C tests, script tests, 29 emulator scenarios (~6 min)
python3 scripts/build_patched_image.py home-ui
.venv/bin/python scripts/preview_link_status.py --only all-pass   # one scenario -> output/link-status/
.venv/bin/python scripts/preview_menu.py --image firmware/patched/home-ui/LPM-10C_TX_V2.0.7_260907.bin --out output/home-ui
python3 scripts/check_dead_code.py firmware/patched/home-ui/LPM-10C_TX_V2.0.7_260907.bin
make format                                   # clang-format for src/ and tests/
```

- Change a screen: render it with the previews and attach the PNGs to the pull
  request.
- Change the network checks: add or adjust a scenario in
  `scripts/preview_link_status.py`. The simulated network there is the only
  place most failure paths can be exercised.
- Change the text the patch draws: rerun `python3 scripts/gen_subset_font.py`.
  A test fails if a drawn character is missing from the font subset.
- Reuse freed space only where `check_dead_code.py` reports it as safe.

## Testing on a tester

Emulator tests do not replace a real unit. Before proposing a change as tested
on hardware:

1. Keep a copy of the official V2.0.7 update to go back to.
2. Install the image from **Windows** (hold **M** while powering on, copy
   `LPM-10C_TX_V2.0.7_260907.bin` to the drive). FNIRSI warns against updating
   from macOS.
3. Check every screen you touched, plus power-off and power-on, so the settings
   still persist.
4. Put the image SHA-256 in the pull request. A maintainer adds it to
   `tested-builds.sha256` once it is confirmed; the release notes say whether
   the released image is one of those.

Do not test factory reset on a unit whose settings you care about.

## Commits and pull requests

- Imperative subjects of 72 characters or fewer: `Add DHCPv6 rapid commit`, `Fix scroll bar height`.
- One logical change per commit. Run `make test` before pushing.
- In the pull request: why the change is needed, what changes on screen or on
  the wire, how it was validated (emulator scenarios, hardware model and
  firmware SHA-256 when tested on a unit), and screenshots for visible changes.
- Update `CHANGELOG.md` under an `Unreleased` heading; the maintainer renames
  it to the version when tagging.

## Releases

Maintainers only:

1. Rename `## Unreleased` in `CHANGELOG.md` to `## vX.Y.Z` and commit.
2. `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. The Release workflow builds, runs the tests, writes the patch and publishes
   it with the changelog section and build facts. It downloads the official
   FNIRSI update from the URL in the workflows; if FNIRSI moves it, set the
   repository variable `VENDOR_ZIP_URL` to the new location. The workflow
   checks the SHA-256 of the TX image inside it.

By contributing, you agree that your contributions are licensed under the GNU
General Public License v3.0 or later (`LICENSE`), with the additional permission
for the Terminus font data in `NOTICE`.

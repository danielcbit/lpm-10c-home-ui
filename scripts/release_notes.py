#!/usr/bin/env python3
"""Release notes for a tag: its CHANGELOG.md section, then the build facts.

  python3 scripts/release_notes.py v1.0.0 > notes.md

Run after the release patch is in dist/ (see .github/workflows/release.yml). The
notes say whether the built image is byte-identical to one confirmed on a
tester (tested-builds.sha256), and how to apply the patch.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def changelog_section(tag):
    lines, out, inside = (ROOT / 'CHANGELOG.md').read_text().splitlines(), [], False
    for line in lines:
        if line.startswith('## '):
            if inside:
                break
            inside = line[3:].split()[0].strip('[]') == tag
            continue
        if inside:
            out.append(line)
    if not inside and not out:
        raise SystemExit(f'CHANGELOG.md has no "## {tag}" section')
    return '\n'.join(out).strip()


def tested(sha):
    for line in (ROOT / 'tested-builds.sha256').read_text().splitlines():
        if line.startswith('#'):
            continue
        if line.split() and line.split()[0] == sha:
            return line.split(None, 1)[1].strip() if len(line.split()) > 1 else 'confirmed'
    return None


def tool_version(command):
    try:
        return subprocess.run(command, capture_output=True, text=True, check=True).stdout.splitlines()[0].strip()
    except (OSError, subprocess.CalledProcessError, IndexError):
        return 'unknown'


def main():
    tag = sys.argv[1]
    patch_file = ROOT / 'dist' / f'lpm-10c-home-ui-{tag}.patch.json'
    patch = json.loads(patch_file.read_text())
    sha = patch['output_sha256']
    confirmed = tested(sha)
    changed = sum(len(r['bytes']) // 2 for r in patch['ranges'])
    print(changelog_section(tag))
    print(f'''
## Install

This release does not contain FNIRSI's firmware. It contains a patch for the
official **LPM-10C V2.0.7** update, which you download from FNIRSI yourself.

1. Download the LPM-10C V2.0.7 update from FNIRSI: <https://www.fnirsi.com.cn/uploads/file1/20261010/d68467f735b832bc57860df7f230ada41791625743186.zip>
2. Download `{patch_file.name}` and `lpm_patch.py` from this release.
3. Run `python3 lpm_patch.py apply d68467f735b832bc57860df7f230ada41791625743186.zip {patch_file.name}`. It checks the vendor
   file, applies the patch, checks the result and writes `{patch["output_name"]}`.
4. Install it **from Windows**: hold **M** while powering on the transmitter, copy the file
   to the drive that appears, and wait for it to restart. Do not update from macOS (FNIRSI's
   own warning).

To go back, install the unmodified FNIRSI update the same way.

**Use at your own risk.** See the disclaimer in the README: modified firmware can leave the
tester unusable, and nobody but you is responsible for that.

## Build

| | |
| --- | --- |
| Patched image SHA-256 | `{sha}` |
| Vendor image SHA-256 | `{patch["vendor_sha256"]}` |
| Bytes changed | {changed} in {len(patch["ranges"])} ranges, file size unchanged ({patch["size"]} bytes) |
| Confirmed on a tester | {"yes: " + confirmed if confirmed else "**no**: this exact image has not been installed on a tester yet"} |
| Toolchain | {tool_version([os.environ.get("CLANG", "clang"), "--version"])}; {tool_version(["ld.lld", "--version"])} |
''')
    return 0


if __name__ == '__main__':
    sys.exit(main())

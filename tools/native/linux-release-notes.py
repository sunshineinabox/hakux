#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate Linux release notes from CHANGELOG.md, with install and build links.
Fail if the version has no changelog section.
    python3 tools/native/linux-release-notes.py <version> <commit> [<previous tag> [<run url>]]
"""
from pathlib import Path
import re
import sys

if not 3 <= len(sys.argv) <= 5:
    sys.exit(__doc__)
version, commit = sys.argv[1], sys.argv[2]
previous = sys.argv[3] if len(sys.argv) > 3 else ''
run = sys.argv[4] if len(sys.argv) > 4 else ''
repo = 'https://github.com/portare-ch/hakuX-lr'
tag = f'linux-v{version}'

changelog = (Path(__file__).resolve().parents[2] / 'CHANGELOG.md').read_text()
section = re.search(rf'^## {re.escape(tag)}(?:[ \t][^\n]*)?\n(.*?)(?=^## |\Z)', changelog, re.M | re.S)
if not section:
    sys.exit(f'CHANGELOG.md has no "## {tag}" section')

on_portareos = ('On PortareOS, extract the `aarch64` archive and copy `hakux_libretro.so` and '
                '`hakux_libretro.info` from `lib/libretro/` to `/storage/cores/`, then restart. '
                'These files override the bundled core, including after OS updates; '
                'remove both and restart to restore it. Back up `/storage/saves/xbox/hdd/` before updating.')
built = f'Built from [{commit[:10]}]({repo}/commit/{commit})'
built += f' by [CI]({run}).' if run else '.'
print('For ARM64 Linux, including PortareOS on the Retroid Pocket Nova, and AMD64 (x86-64) Linux.\n')
print(section.group(1).strip() + '\n')
print('### Install\n')
print(on_portareos + '\n')
print('For other ARM64 Linux systems, follow the '
      f'[RetroArch installation guide]({repo}/blob/{tag}/README.md#play-on-other-aarch64-linux-systems).\n')
print('For AMD64 (x86-64) Linux, download the `x86_64` archive and follow the '
      f'[KVM setup and RetroArch installation guide]({repo}/blob/{tag}/README.md#play-on-amd64-x86-64-linux-systems).\n')
print(built + ' `BUILD-INFO` lists the source revision and pinned dependencies.')
if previous:
    print(f'\n[Full changelog]({repo}/compare/{previous}...{tag})')

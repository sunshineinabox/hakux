#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the release archive and write its SHA-256 checksum.

Check libretro exports, dependencies, DSP JIT, and ABI requirements:
ARM64: glibc <= 2.38, GLIBCXX <= 3.4.31.
AMD64: glibc <= 2.35, GLIBCXX <= 3.4.30 (Ubuntu 22.04 sysroot).

Usage: python3 tools/native/linux-verify.py <version>
"""
from pathlib import Path
import ctypes
import hashlib
import importlib.util
import platform
import re
import struct
import subprocess
import sys
import tarfile

version = sys.argv[1] if len(sys.argv) == 2 else sys.exit(__doc__)
root = Path(__file__).resolve().parents[2]
package = root / 'build/package'
name = f'hakux-{version}'
arch = platform.machine()
machines = {'aarch64': 183, 'x86_64': 62}
assert arch in machines, f'unsupported architecture: {arch}'
archive = package / f'{name}-{arch}.tar.gz'
core = package / name / 'lib/libretro/hakux_libretro.so'
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
assert not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root)
assert struct.unpack_from('<H', core.read_bytes(), 18)[0] == machines[arch], f'not {arch}'
with tarfile.open(archive) as tar:
    names = tar.getnames()
    assert f'{name}/lib/libretro/hakux_libretro.so' in names
    assert f'{name}/lib/libretro/hakux_libretro.info' in names
    assert all(m.uid == 0 and m.gid == 0 for m in tar.getmembers())
    info = tar.extractfile(f'{name}/lib/libretro/hakux_libretro.info').read().decode()
    assert f'display_version = "{version}"' in info
    assert 'supported_extensions = "iso|chd"' in info
    build_info = tar.extractfile(f'{name}/BUILD-INFO').read().decode()
    assert 'source ' + revision in build_info
    assert 'architecture ' + arch in build_info
    assert 'DSP JIT on' in build_info, 'the core has no DSP JIT'
    assert 'libchdr' in build_info
    assert tar.extractfile(f'{name}/lib/libretro/hakux_libretro.so').read() == core.read_bytes()


class SystemInfo(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char_p), ('version', ctypes.c_char_p),
                ('extensions', ctypes.c_char_p), ('need_fullpath', ctypes.c_bool),
                ('block_extract', ctypes.c_bool)]


library = ctypes.CDLL(str(core))
system_info = SystemInfo()
library.retro_get_system_info(ctypes.byref(system_info))
assert system_info.version == version.encode(), system_info.version
assert system_info.extensions == b'iso|chd'
spec = importlib.util.spec_from_file_location('release_checks', root / 'tests/native/libretro/check.py')
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)
exports = checks.dynamic_exports(core)
assert exports and all(n.startswith('retro_') for n in exports), exports
dynamic = subprocess.check_output(['readelf', '-d', str(core)], text=True)
assert '(RUNPATH)' not in dynamic and '(RPATH)' not in dynamic
assert 'libSDL3' not in dynamic and 'libaaudio' not in dynamic
versions = subprocess.check_output(['readelf', '--version-info', str(core)], text=True)
glibc = sorted({tuple(map(int, x.split('.'))) for x in re.findall(r'GLIBC_([0-9.]+)', versions)})
glibcxx = sorted({tuple(map(int, x.split('.'))) for x in re.findall(r'GLIBCXX_([0-9.]+)', versions)})
glibc_max, glibcxx_max = {'aarch64': ((2, 38), (3, 4, 31)), 'x86_64': ((2, 35), (3, 4, 30))}[arch]
assert glibc[-1] <= glibc_max, glibc[-1]
assert glibcxx[-1] <= glibcxx_max, glibcxx[-1]
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
(package / (archive.name + '.sha256')).write_text(f'{digest}  {archive.name}\n')
print(f'PASS: release {version}, source {revision}, {arch}, iso|chd, {len(exports)} retro_* exports, no RUNPATH')
print('PASS: runtime GLIBC_' + '.'.join(map(str, glibc[-1])) + ', GLIBCXX_' + '.'.join(map(str, glibcxx[-1])))
print(f'{digest}  {archive.name} ({archive.stat().st_size} bytes)')

#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the failed-commit regression test on xboxrun.

Assembles kernel_mm.s for i386 with clang and ld.lld; skips (exit 77) if
either tool is unavailable.
Usage: kernel_mm.py <xboxrun's directory> [clang]
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
# Thunk order: DbgPrint, NtAllocateVirtualMemory, NtFreeVirtualMemory.
IMPORTS = [8, 184, 199]


def tool(*names):
    for name in names:
        if name and shutil.which(name):
            return shutil.which(name)
    return None


def main():
    build = Path(sys.argv[1]).resolve()
    cc = tool(sys.argv[2] if len(sys.argv) > 2 else None, os.environ.get('CC'), 'clang', 'clang-19')
    ld = tool('ld.lld', 'ld.lld-19')
    if not cc or not ld:
        print('SKIP: no clang or ld.lld for i386')
        return 77
    spec = importlib.util.spec_from_file_location('check', HERE / 'check.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    with tempfile.TemporaryDirectory(prefix='hakux-kernel-mm-') as folder:
        root = Path(folder)
        subprocess.run([cc, '-target', 'i386-linux-gnu', '-c', HERE / 'kernel_mm.s', '-o', root / 'k.o'], check=True)
        subprocess.run([ld, '-m', 'elf_i386', '--image-base=0x10000', '-Ttext=0x11000', '-e', '_start',
                        '--oformat=binary', root / 'k.o', '-o', root / 'k.bin'], check=True)
        check.disc(root / 'kernel_mm.iso', (root / 'k.bin').read_bytes(), IMPORTS)
        env = dict(os.environ, NC_TIMEOUT='10')
        check.run([build / 'xboxrun', '--headless', root / 'kernel_mm.iso', root / 'data'], 0,
                  ('kmm: all passed', 'guest executed HLT'), env)
    return 0


if __name__ == '__main__':
    sys.exit(main())

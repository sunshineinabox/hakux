#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""mod-to-cht.py: a mod.txt's patches by virtual address land at the XBE
file offsets its section headers give (the header page too), "old -> new"
bytes are checked, a title update's section is left out, and what an
xbe-patch-v1 entry can't hold is refused."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

TOOL = Path(__file__).resolve().parent / 'mod-to-cht.py'


def check(ok, what):
    if not ok:
        raise SystemExit(f'FAIL: {what}')
    print(f'PASS: {what}')


def xbe():
    """an XBE at 0x10000 with one section: VA 0x11000, raw 0x1000, 0x2000 bytes"""
    d = bytearray(0x3000)
    d[0:4] = b'XBEH'
    struct.pack_into('<II', d, 0x104, 0x10000, 0x400)        # base, sizeof_headers
    struct.pack_into('<II', d, 0x11C, 1, 0x10200)             # sections, their headers' VA
    struct.pack_into('<IIIII', d, 0x200, 6, 0x11000, 0x2000, 0x1000, 0x2000)
    for i in range(0x1000, 0x3000):
        d[i] = i & 0xFF
    return bytes(d)


def convert(tmp, text, stock):
    (tmp / 'mod.txt').write_text(text)
    (tmp / 'default.xbe').write_bytes(stock)
    r = subprocess.run([sys.executable, str(TOOL), str(tmp / 'mod.txt'), str(tmp / 'default.xbe')],
                       capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def main():
    stock = xbe()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        rc, out, err = convert(tmp, 'name: 60 fps\ndescription: Faster.\n\n[default.xbe]\n'
                               '0x00011010: AA BB\n0x00011020: 20 21 -> 90 90  # a check\n'
                               '0x00010300: 01\n\n[$u\\default.xbe]\n0x00011000: 00\n', stock)
        check(rc == 0, f'converts ({err.strip()})')
        code = re.search(r'cheat0_code = "([^"]+)"', out).group(1)
        check(code.startswith('xbe-patch-v1 ') and '+00000300:01' in code and '+00001010:AABB' in code and
              '+00001020:9090' in code, f'addresses at their file offsets: {code}')
        check('cheat0_desc = "60 fps [restart]"' in out and '# Faster.' in out and
              'title-update patches are left out' in out, 'the name, description and the left-out update')
        for text, why in (('[default.xbe]\n0x00011020: 00 00 -> 90 90\n', 'other bytes there'),
                          ('refresh: 120\n[default.xbe]\n0x00011010: AA\n', 'refresh: 120'),
                          ('[default.xbe]\n0x00020000: AA\n', "not in the XBE's file"),
                          ('[default.xbe]\n0x00011010: AA\n[game.xbe]\n0x00011010: AA\n', 'patches 2 XBEs')):
            rc, out, err = convert(tmp, text, stock)
            check(rc != 0 and not out and why in err, f'refused: {err.strip()}')


if __name__ == '__main__':
    main()

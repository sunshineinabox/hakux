#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""xbe-to-cht.py: an IPS, a BPS (made with Floating IPS) and the patched XBE
give the same .cht for the same edits; close edits joined, long ones split
at 64 bytes; what xbe-patch-v1 can't hold refused. Without Floating IPS
only the paired-XBE cases run."""
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

TOOL = Path(__file__).resolve().parent / 'xbe-to-cht.py'


def convert(*args):
    r = subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def check(ok, what):
    if not ok:
        raise SystemExit(f'FAIL: {what}')
    print(f'PASS: {what}')


def main():
    rng = random.Random(167)
    stock = bytearray(b'XBEH' + bytes(rng.randrange(256) for _ in range(0x3FFC)))
    patched = bytearray(stock)
    for off, data in ((0x123, b'\x90\x90'), (0x128, b'\xeb'),        # 4 bytes apart: one run
                      (0x200, b'\x70'), (0x210, b'\x70'),            # 15 apart: two
                      (0x1000, bytes(range(100))),                   # 100: 64 and 36
                      (0x3FFF, b'\x00')):                            # the last byte
        for i, b in enumerate(data):
            patched[off + i] = b ^ 0xFF if stock[off + i] == b else b
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'stock.xbe').write_bytes(stock)
        (tmp / 'patched.xbe').write_bytes(patched)
        rc, paired, err = convert(tmp / 'stock.xbe', tmp / 'patched.xbe')
        check(rc == 0, f'paired XBEs convert ({err.strip()})')
        code = re.search(r'cheat0_code = "([^"]+)"', paired).group(1)
        head = f'xbe-patch-v1 {zlib.crc32(stock):08X}:{len(stock):08X}'
        offsets = [int(m, 16) for m in re.findall(r'\+([0-9A-F]{8}):', code)]
        check(code.startswith(head) and offsets == [0x123, 0x200, 0x210, 0x1000, 0x1040, 0x3FFF],
              f'runs joined and split as expected: {offsets}')
        check(re.search(r'\+00000123:([0-9A-F]+)', code).group(1) == patched[0x123:0x129].hex().upper(),
              'a joined run carries the unchanged bytes between')
        check(paired.startswith('cheats = 1\ncheat0_desc = "Widescreen (16:9) [restart]"\ncheat0_handler = 0\n')
              and paired.endswith('cheat0_enable = false\n'), 'the .cht entry: emulator-handled, off')
        rc, noted, err = convert(tmp / 'stock.xbe', tmp / 'patched.xbe', '--note', 'Game (USA), default.xbe',
                                 '--note', 'Patch: someone', '--desc', '60 fps [restart]')
        check(rc == 0 and noted.startswith('# Game (USA), default.xbe\n# Patch: someone\ncheats = 1\n') and
              'cheat0_desc = "60 fps [restart]"' in noted and code in noted, 'notes as comments, another name')
        flips = shutil.which(os.environ.get('FLIPS', 'flips'))
        if flips:
            for kind in ('ips', 'bps'):
                patch = tmp / f'p.{kind}'
                subprocess.run([flips, '--create', f'--{kind}', tmp / 'stock.xbe', tmp / 'patched.xbe', patch],
                               capture_output=True, check=True)
                rc, out, err = convert(tmp / 'stock.xbe', patch, '--flips', flips)
                check(rc == 0 and out == paired, f'{kind.upper()} gives the paired XBEs\' .cht ({err.strip()})')
            # a BPS made for another XBE: Floating IPS refuses it
            other = bytearray(stock)
            other[0x500] ^= 1
            (tmp / 'other.xbe').write_bytes(other)
            rc, out, err = convert(tmp / 'other.xbe', tmp / 'p.bps', '--flips', flips)
            check(rc != 0 and not out, 'a BPS for another revision is refused')
            # an IPS over the magic (a paired file without it isn't an XBE)
            (tmp / 'magic.xbe').write_bytes(b'XBEX' + bytes(patched[4:]))
            subprocess.run([flips, '--create', '--ips', tmp / 'stock.xbe', tmp / 'magic.xbe', tmp / 'magic.ips'],
                           capture_output=True, check=True)
            rc, out, err = convert(tmp / 'stock.xbe', tmp / 'magic.ips', '--flips', flips)
            check(rc != 0 and not out and "the XBE's magic" in err, f'refused: {err.strip()}')
        else:
            print('SKIP: IPS and BPS (no Floating IPS)')
        # a long edit's last part takes the edit just after it: 43 times a
        # 65-byte edit and a 1-byte one 2 bytes on fit 86 runs (joining
        # before splitting made 129, refused)
        dense = bytearray(stock)
        for i in range(43):
            at = 0x200 + 0x100 * i
            for j in list(range(65)) + [67]:
                dense[at + j] ^= 0xFF
        (tmp / 'dense.xbe').write_bytes(dense)
        rc, out, err = convert(tmp / 'stock.xbe', tmp / 'dense.xbe')
        n = len(re.findall(r'\+[0-9A-F]{8}:', out))
        check(rc == 0 and n == 86, f'packed as they come: {n} runs ({err.strip()})')
        for name, data, why in (('longer.xbe', bytes(patched) + b'\0', 'keeps the size'),
                                ('same.xbe', bytes(stock), 'changes nothing'),
                                ('many.xbe', bytes(b ^ 1 if 4 <= i < 0x3FFF and i % 16 == 4 else b
                                                   for i, b in enumerate(stock)), 'holds 128'),
                                ('junk.bin', b'JUNK' + bytes(16), 'not an IPS or BPS patch or an XBE')):
            (tmp / name).write_bytes(data)
            rc, out, err = convert(tmp / 'stock.xbe', tmp / name)
            check(rc != 0 and not out and why in err, f'refused: {err.strip()}')


if __name__ == '__main__':
    main()

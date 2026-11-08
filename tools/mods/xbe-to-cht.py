#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write a RetroArch .cht for hakuX-lr's XBE patches from a stock XBE and
either an IPS or BPS patch (applied with Floating IPS) or the patched XBE.

  xbe-to-cht.py stock.xbe widescreen.bps > "Game (USA).cht"
  xbe-to-cht.py stock.xbe patched.xbe --desc "60 fps [restart]"

The .cht holds one emulator-handled entry, off: an xbe-patch-v1 code with
the stock XBE's CRC-32 and size and the bytes that differ as runs at their
file offsets (native/core/xbe_patch.h). Load it under Quick Menu, Cheats,
turn it on, Apply Changes, then Restart. Equivalent IPS, BPS and patched
XBE inputs give the same file.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zlib

RUN_MAX = 64            # bytes a run
RUNS_MAX = 128          # runs an entry
BRIDGE = 5              # unchanged bytes taken into a run rather than a new one ("+OFFSET:" is 10 digits)
WIDER = (8, 16, 32, 63)  # gaps tried after it when the runs don't fit RUNS_MAX


def fail(why):
    sys.exit(f'xbe-to-cht: {why}')


def flips_path(given):
    for name in (given, os.environ.get('FLIPS'), 'flips', 'flips-linux'):
        if name and (shutil.which(name) or os.path.isfile(name)):
            return shutil.which(name) or name
    fail('IPS and BPS patches are applied with Floating IPS: install flips, or name it with --flips or FLIPS')


def apply_patch(stock_path, patch_path, flips):
    """the stock XBE patched by Floating IPS"""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'patched.xbe')
        r = subprocess.run([flips, '--apply', '--exact', patch_path, stock_path, out],
                           capture_output=True, text=True)
        if r.returncode or not os.path.isfile(out):
            fail(f'Floating IPS could not apply {patch_path}: {(r.stdout + r.stderr).strip()}')
        with open(out, 'rb') as f:
            return f.read()


def spans(a, b):
    """[start, end) of each stretch of bytes that differ"""
    out, n, chunk = [], len(a), 4096
    at = 0
    while at < n:
        if a[at:at + chunk] == b[at:at + chunk]:
            at += chunk
            continue
        end = min(at + chunk, n)
        i = at
        while i < end:
            if a[i] != b[i]:
                j = i
                while j < n and a[j] != b[j]:
                    j += 1
                if out and out[-1][1] == i:
                    out[-1][1] = j
                else:
                    out.append([i, j])
                i = j
            else:
                i += 1
        at = max(end, i)
    return out


def runs(stock, patched, bridge=BRIDGE):
    """the differing stretches packed into runs of up to RUN_MAX bytes as
    they come: a stretch bridge or fewer unchanged bytes after the run goes
    into it while there's room (a long stretch's last part takes the edits
    after it too)"""
    out, cur = [], None             # cur: [start, end) of the run being filled
    for s, e in spans(stock, patched):
        while s < e:
            if cur and s - cur[1] <= bridge and s < cur[0] + RUN_MAX:
                cur[1] = min(e, cur[0] + RUN_MAX)
            else:
                if cur:
                    out.append(cur)
                cur = [s, min(e, s + RUN_MAX)]
            s = cur[1]
    if cur:
        out.append(cur)
    return [(a, patched[a:b]) for a, b in out]


def code(stock, patched):
    if stock[:4] != b'XBEH':
        fail('the stock file is not an XBE')
    if len(patched) != len(stock):
        fail(f'the patched XBE is {len(patched)} bytes, the stock one {len(stock)}: '
             'xbe-patch-v1 keeps the size')
    found = runs(stock, patched)
    for bridge in WIDER:                # sparse edits: wider gaps before refusing
        if len(found) <= RUNS_MAX:
            break
        found = runs(stock, patched, bridge)
    if not found:
        fail('the patch changes nothing')
    if found[0][0] < 4:
        fail("the patch changes the XBE's magic (bytes 0-3)")
    if len(found) > RUNS_MAX:
        fail(f'{len(found)} runs: xbe-patch-v1 holds {RUNS_MAX}')
    text = f'xbe-patch-v1 {zlib.crc32(stock):08X}:{len(stock):08X}'
    return text + ''.join(f'+{off:08X}:{data.hex().upper()}' for off, data in found)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('stock', help="the game's XBE as the disc has it")
    ap.add_argument('patch', help='an IPS or BPS patch of it, or the patched XBE')
    ap.add_argument('--desc', default='Widescreen (16:9) [restart]', help='the entry\'s name in the menu')
    ap.add_argument('--flips', help='Floating IPS (default: flips on the PATH, or $FLIPS)')
    ap.add_argument('--note', action='append', default=[],
                    help='a comment line at the top (the game, the patch\'s author and source...); repeatable')
    args = ap.parse_args()
    if '"' in args.desc:
        fail('the description cannot hold a double quote')
    if any('\n' in n for n in args.note):
        fail('a note is one line')
    with open(args.stock, 'rb') as f:
        stock = f.read()
    with open(args.patch, 'rb') as f:
        head = f.read(5)
    if head[:5] == b'PATCH' or head[:4] == b'BPS1':
        patched = apply_patch(args.stock, args.patch, flips_path(args.flips))
    elif head[:4] == b'XBEH':
        with open(args.patch, 'rb') as f:
            patched = f.read()
    else:
        fail(f'{args.patch} is not an IPS or BPS patch or an XBE')
    sys.stdout.write(''.join(f'# {n}\n' for n in args.note) +
                     'cheats = 1\n'
                     f'cheat0_desc = "{args.desc}"\n'
                     'cheat0_handler = 0\n'
                     f'cheat0_code = "{code(stock, patched)}"\n'
                     'cheat0_enable = false\n')


if __name__ == '__main__':
    main()

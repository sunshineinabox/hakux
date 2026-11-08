#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write a RetroArch .cht (an xbe-patch-v1 entry) from an Android-format
mod (mod.txt: patches by virtual address, patches/mods/README.md: hakux-lr-patches)
and the stock XBE it patches.

  mod-to-cht.py patches/mods/4D530004.9/60fps/mod.txt default.xbe > "Halo (USA)/60 fps.cht"

The XBE's section headers map each address to its file offset; "old -> new"
bytes are checked against the stock file. A [$u\\...] section (a title
update) is left out: xbe-patch-v1 patches disc XBEs. The runs are packed
as xbe-to-cht.py packs them.
"""
import argparse
import importlib.util
from pathlib import Path
import struct
import sys

_spec = importlib.util.spec_from_file_location('xbe_to_cht', Path(__file__).resolve().parent / 'xbe-to-cht.py')
xbe_to_cht = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(xbe_to_cht)


def fail(why):
    sys.exit(f'mod-to-cht: {why}')


def xbe_offset(xbe, va, n):
    """va's file offset in the XBE: its headers (up to the first section's
    data, past sizeof_headers: a mod's code can go there) or a section's raw
    data, as native/core/mods.c maps it"""
    base, size_hdr = struct.unpack_from('<II', xbe, 0x104)
    nsec, sec_va = struct.unpack_from('<II', xbe, 0x11C)
    secs = []
    for i in range(nsec):
        off = sec_va - base + 56 * i
        _, sva, vsize, raw, rsize = struct.unpack_from('<IIIII', xbe, off)
        secs.append((sva, raw, rsize))
    hdr_end = max(min([0x1000] + [raw for _, raw, _ in secs]), size_hdr)
    if base <= va and va - base + n <= hdr_end:
        return va - base
    for sva, raw, rsize in secs:
        if sva <= va and va - sva + n <= rsize:
            return raw + va - sva
    return None


def parse(path):
    """the mod's fields and its sections' patches: [(xbe, va, old, new)]"""
    fields, patches, section = {}, [], None
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith('[') and s.endswith(']'):
            section = s[1:-1]
            continue
        if section is None:
            k, _, v = s.partition(':')
            fields[k.strip()] = v.strip()
            continue
        va, _, rest = s.partition(':')
        rest = rest.split('#')[0]
        old, arrow, new = rest.partition('->')
        if not arrow:
            old, new = '', old
        patches.append((section, int(va, 0), bytes.fromhex(old.replace(' ', '')),
                        bytes.fromhex(new.replace(' ', ''))))
    return fields, patches


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('mod', help='the mod.txt')
    ap.add_argument('xbe', help='the stock XBE its section names')
    ap.add_argument('--note', action='append', default=[], help='a comment line at the top; repeatable')
    args = ap.parse_args()
    fields, patches = parse(args.mod)
    if 'refresh' in fields:
        fail(f'the mod sets refresh: {fields["refresh"]}, which xbe-patch-v1 entries cannot carry')
    stock = Path(args.xbe).read_bytes()
    if stock[:4] != b'XBEH':
        fail(f'{args.xbe} is not an XBE')
    disc = [p for p in patches if not p[0].startswith('$u')]
    skipped = len(patches) - len(disc)
    names = {p[0].lower() for p in disc}
    if len(names) != 1:
        fail(f'the mod patches {len(names)} XBEs ({", ".join(sorted(names)) or "none"}); one .cht holds one')
    patched = bytearray(stock)
    for section, va, old, new in disc:
        off = xbe_offset(stock, va, len(new))
        if off is None:
            fail(f'[{section}] {va:#010x}: not in the XBE\'s file')
        if old and stock[off:off + len(old)] != old:
            fail(f'[{section}] {va:#010x}: other bytes there (another revision?)')
        patched[off:off + len(new)] = new
    code = xbe_to_cht.code(stock, bytes(patched))
    notes = list(args.note)
    if fields.get('description'):
        notes.append(fields['description'])
    if skipped:
        notes.append(f'Its {skipped} title-update patches are left out (xbe-patch-v1 patches disc XBEs).')
    desc = f'{fields.get("name", Path(args.mod).parent.name)} [restart]'
    if '"' in desc:
        fail('the name cannot hold a double quote')
    sys.stdout.write(''.join(f'# {n}\n' for n in notes) +
                     'cheats = 1\n'
                     f'cheat0_desc = "{desc}"\n'
                     'cheat0_handler = 0\n'
                     f'cheat0_code = "{code}"\n'
                     'cheat0_enable = false\n')


if __name__ == '__main__':
    main()

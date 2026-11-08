#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""The GP probe's recording (native/dev/apu/dsp/gp_probe.c, NC_APU_GP_PROBE)
read back: for each tone, which bin it went into, and the level and phase
of that tone in every output the GP and EP gave, each output's channels
apart; then the DSPs' Y memories as the probe began (their coefficient
tables, to read beside the measurement: the EP's stereo downmix sits there).

    python3 apu-gp-probe.py <probe directory> [--all]

Recording one (a title's own DSP image runs only with the full DSP): in
RetroArch, with the core started with NC_APU_DSP=1 NC_APU_GP_PROBE=<dir>
and NC_APU_GP_PROBE_AT=<seconds> in its environment (PortareOS: runemu.sh
passes its environment on; a test build of the core can be bound over
/tmp/cores/hakux_libretro.so). xboxrun --headless doesn't get Halo past its
start on Linux. The title's own sounds are replaced by the tones while the
probe runs, about 17 s from its start; it logs "gp probe: done".

Outputs: the GP's mix buffer after its frame ("mix" and the bin), and each
output FIFO write stream ("GP-FIFO n", "EP-FIFO n"). A FIFO's frames are
read as 32-bit words in channels of 32 samples a frame when they divide
so, as 16-bit stereo for the EP's FIFO 0 (the analog output's 256 frames
every 8 GP frames). Levels are least-squares sine fits (sine, cosine and
DC) over each tone's frames past its first 16 (its settling), relative to
the tone's 0.25 of full scale; phases are relative to the tone's own.
Responses under -80 dB are left out unless --all.
"""
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

MAGIC = 0x46504750
WORDS = 1024
SAMPLES = 32
AMPLITUDE = 0.25
SETTLE = 16
NAMES = {0: 'front left', 1: 'front right', 2: 'centre', 3: 'LFE', 4: 'back left', 5: 'back right',
         6: '3D front left', 7: '3D front right', 8: '3D back left', 9: '3D back right', 10: 'I3DL2'}


def sext24(w):
    w &= 0xFFFFFF
    return (w - 0x1000000 if w & 0x800000 else w) / float(1 << 23)


def records(path):
    data = Path(path).read_bytes()
    at = 0
    while at + 28 <= len(data):
        magic, seg, binv, nblobs, blen = struct.unpack_from('<IIi', data, at) + struct.unpack_from('<II', data, at + 20)
        hz, frame = struct.unpack_from('<fI', data, at + 12)
        if magic != MAGIC:
            raise SystemExit(f'bad record at {at}')
        at += 28
        mix = struct.unpack_from(f'<{WORDS}I', data, at)
        at += WORDS * 4
        blobs = []
        end = at + blen
        while at < end:
            dsp, index, n = data[at], data[at + 1], struct.unpack_from('<I', data, at + 4)[0]
            blobs.append((chr(dsp), index, data[at + 8:at + 8 + n]))
            at += 8 + n
        yield seg, binv, hz, frame, mix, blobs


def fit(y, f, start):
    """The amplitude and phase (degrees) of the sine at f in y[start:]"""
    m = [[0.0] * 4 for _ in range(3)]
    for i, v in enumerate(y[start:], start):
        a, b = math.sin(2 * math.pi * f * i / 48000), math.cos(2 * math.pi * f * i / 48000)
        row = (a, b, 1.0, v)
        for r in range(3):
            for c in range(4):
                m[r][c] += row[r] * row[c]
    for c in range(3):
        if m[c][c] == 0:
            return 0.0, 0.0
        for r in range(c + 1, 3):
            k = m[r][c] / m[c][c]
            for j in range(c, 4):
                m[r][j] -= k * m[c][j]
    x = [0.0] * 3
    for r in (2, 1, 0):
        x[r] = (m[r][3] - sum(m[r][j] * x[j] for j in range(r + 1, 3))) / m[r][r]
    return math.hypot(x[0], x[1]), math.degrees(math.atan2(x[1], x[0]))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    show_all = '--all' in sys.argv
    segs = {}                                   # seg -> (bin, hz)
    streams = defaultdict(lambda: defaultdict(list))    # seg -> name -> samples
    sizes = defaultdict(set)
    for seg, binv, hz, frame, mix, blobs in records(Path(sys.argv[1]) / 'probe.bin'):
        segs[seg] = (binv, hz)
        for b in range(WORDS // SAMPLES):
            streams[seg][f'mix {b:2d}'].extend(sext24(w) for w in mix[b * SAMPLES:(b + 1) * SAMPLES])
        for dsp, index, raw in blobs:
            name = f'{"GP" if dsp == "G" else "EP"}-FIFO {index}'
            sizes[name].add(len(raw))
            if dsp == 'E' and index == 0 and len(raw) % 4 == 0 and len(raw) // 4 >= 64:
                pairs = struct.unpack(f'<{len(raw) // 2}h', raw)
                for ch in range(2):
                    streams[seg][f'{name} ch{ch}'].extend(v / 32768.0 for v in pairs[ch::2])
            elif len(raw) % (4 * SAMPLES) == 0:
                words = struct.unpack(f'<{len(raw) // 4}I', raw)
                channels = len(words) // SAMPLES
                for ch in range(channels):
                    streams[seg][f'{name} ch{ch}'].extend(sext24(w) for w in words[ch::channels])
            else:
                streams[seg][f'{name} raw{len(raw)}'].extend(sext24(w) for w in struct.unpack(f'<{len(raw) // 4}I', raw[:len(raw) // 4 * 4]))
    print('FIFO write sizes (bytes):', {k: sorted(v) for k, v in sorted(sizes.items())})
    for seg in sorted(segs):
        binv, hz = segs[seg]
        label = 'silence' if binv < 0 else f'bin {binv} ({NAMES.get(binv, "effect send")}) at {hz:.0f} Hz'
        rows = []
        for name, y in sorted(streams[seg].items()):
            if len(y) < 4 * SAMPLES:
                continue
            f = hz if binv >= 0 else 1000.0
            per_frame = SAMPLES * 8 if name.startswith('EP-FIFO 0') else SAMPLES
            start = SETTLE * SAMPLES if per_frame == SAMPLES else min(len(y) // 4, SETTLE * SAMPLES)
            amp, ph = fit(y, f, start)
            level = 20 * math.log10(amp / AMPLITUDE) if amp > 0 else -999
            if show_all or level > -80:
                rows.append(f'    {name:16s} {level:+8.2f} dB  phase {ph:+7.1f}')
        print(f'{label}:')
        print('\n'.join(rows) if rows else '    (nothing above -80 dB)')


def tables(directory):
    """The loaded words of each DSP's Y memory (its P, X and Y follow one
    another in <dsp>_mem.bin: 4096, 4096 and 2048 words), as fractions"""
    for name in ('gp', 'ep'):
        path = Path(directory) / f'{name}_mem.bin'
        if not path.exists():
            continue
        words = struct.unpack(f'<{path.stat().st_size // 4}I', path.read_bytes())
        y = [(i, w) for i, w in enumerate(words[8192:8192 + 2048]) if w != 0xCACACACA]
        print(f'{name.upper()} Y memory: {len(y)} loaded words' + (', the first 32:' if y else ''))
        for i, w in y[:32]:
            f = sext24(w)
            level = f'{20 * math.log10(abs(f)):+.2f} dB' if f else '0'
            print(f'    Y:{i:04x} = {w & 0xFFFFFF:06x}  {f:+.8f}  ({level})')


if __name__ == '__main__':
    main()
    tables(sys.argv[1])

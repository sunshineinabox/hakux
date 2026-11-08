#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the libretro core: its exports, its info, and a synthetic title run through it."""
import argparse
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'linux'))
from check import disc, run  # noqa: E402  (the day-one fixtures)

# A title that relaunches itself once (XLaunchNewImage): with no launch data
# page, it leaves one naming its own XBE and quick-reboots; booted again with
# the page, it halts.
RELAUNCH = (
    b'\xa1\x04\x0f\x01\x00'          # mov eax, [thunk 1]: &LaunchDataPage
    b'\x83\x38\x00'                  # cmp dword [eax], 0
    b'\x75\x10'                      # jne halt
    b'\xc7\x00\x00\x20\x01\x00'      # mov dword [eax], 0x12000: the page below
    b'\x6a\x02'                      # push 2: ReturnFirmwareQuickReboot
    b'\xff\x15\x00\x0f\x01\x00'      # call [thunk 0]: HalReturnToFirmware
    b'\xeb\xfe'                      # jmp $
    b'\xf4'                          # halt: hlt
).ljust(0x1000, b'\0') + struct.pack('<II', 1, 0) + b'\\Device\\CdRom0;default.xbe'

# Simulate C:\xboxdash.xbe opening Xbox Live: without launch data, name
# xodash\xonlinedash.xbe and quick-reboot; with launch data, halt.
DASH_HOP = RELAUNCH[:0x1000] + struct.pack('<II', 0, 0) + b'\\Device\\Harddisk0\\Partition2\\xodash;xonlinedash.xbe'

# Return from Xbox Live to the dashboard's main menu: quick-reboot with
# LDT_LAUNCH_DASHBOARD, reason 0 (main menu) and no launch path.
DASH_BACK = (
    b'\xa1\x04\x0f\x01\x00'          # mov eax, [thunk 1]: &LaunchDataPage
    b'\xc7\x00\x00\x20\x01\x00'      # mov dword [eax], 0x12000: the page below
    b'\x6a\x02'                      # push 2: ReturnFirmwareQuickReboot
    b'\xff\x15\x00\x0f\x01\x00'      # call [thunk 0]: HalReturnToFirmware
    b'\xeb\xfe'                      # jmp $
).ljust(0x1000, b'\0') + struct.pack('<II', 1, 0)

# A title that reads the console's language (ExQueryNonVolatileSetting, which
# logs the EEPROM's video flags, AV region and language), then halts.
SETTINGS = (
    b'\x83\xec\x0c'                  # sub esp, 12: type, value, result length
    b'\x8d\x44\x24\x08\x50'          # push &result length
    b'\x6a\x04'                      # push 4
    b'\x8d\x44\x24\x0c\x50'          # push &value
    b'\x8d\x44\x24\x0c\x50'          # push &type
    b'\x6a\x07'                      # push 7: XC_LANGUAGE
    b'\xff\x15\x00\x0f\x01\x00'      # call [thunk 0]: ExQueryNonVolatileSetting
    b'\xf4'                          # hlt
)


def chd(path, image, hunk=4096, cd=False, lead=0):
    """An uncompressed CHD v5 of image, as libchdr reads one: the 124-byte
    header, the map (each hunk's file offset in hunks, 0 for a hunk of
    zeros), the hunks. cd: the track metadata chdman createcd writes. lead:
    zero bytes before the image (a whole hunks' worth), as a full disc dump
    has them before its game partition."""
    meta = b''
    if cd:
        text = b'TRACK:1 TYPE:MODE1 SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:RW POSTGAP:0\0' % (
            len(image) // 2048)
        meta = struct.pack('>4sIQ', b'CHT2', 1 << 24 | len(text), 0) + text
    hunks = [image[i:i + hunk].ljust(hunk, b'\0') for i in range(0, len(image), hunk)]
    map_off = 124 + len(meta)
    data_off = -(-(map_off + 4 * (lead // hunk + len(hunks))) // hunk) * hunk
    entries, body = [0] * (lead // hunk), b''
    for h in hunks:
        if h.count(0) == hunk:
            entries.append(0)
        else:
            entries.append((data_off + len(body)) // hunk)
            body += h
    header = struct.pack('>8sII4IQQQII20s20s20s', b'MComprHD', 124, 5, 0, 0, 0, 0, lead + len(image), map_off,
                         124 if cd else 0, hunk, 2048, b'', b'', b'')
    out = header + meta + b''.join(struct.pack('>I', e) for e in entries)
    path.write_bytes(out.ljust(data_off, b'\0') + body)


# Use a test NAT port and an unanswered loopback SSDP endpoint so tests
# never change the real router's mappings.
NAT_TEST = 'natport=43080 upnp=127.0.0.1:9'


def cycles(command, env, n, expect=None):
    """The harness's game cycles (HARNESS_CYCLES): each cycle's threads,
    descriptors and memory; the last as the second, up to a margin (the
    first may leave FEX's allocator a heap for the frontend's thread, once:
    nc_cpu_destroy runs there). The signal handlers after each, as before
    the core. expect: a line each cycle logs."""
    # glibc retains worker-thread malloc arenas after the threads exit. A
    # new arena after cycle two reserves 64 MiB and looks like a VmSize leak.
    # Keep one arena from process startup; retain both memory growth limits.
    result = subprocess.run([str(x) for x in command],
                            env=dict(env, HARNESS_CYCLES=str(n), MALLOC_ARENA_MAX='1'), capture_output=True,
                            text=True, timeout=300)
    rows = [tuple(int(v) for v in m) for m in re.findall(
        r'cycle \d+: (\d+) threads, (\d+) descriptors, VmSize (\d+) KiB, VmRSS (\d+) KiB, heap (\d+) KiB, '
        r'malloc (-?\d+) KiB', result.stdout)]
    stops = result.stdout.count('the core stopped in') + result.stderr.count('the core stopped in')
    before = re.findall(r'signals caught before the core: (\w+)', result.stdout)
    caught = re.findall(r'cycle \d+: .*, signals caught (\w+)', result.stdout)
    seen = (result.stdout + result.stderr).count(expect) if expect else n
    if result.returncode or len(rows) != n or stops < n or seen < n:
        print(result.stdout[-4000:], result.stderr[-4000:])
        raise SystemExit(f'FAIL: {n} cycles of {command[2]}: exit {result.returncode}, {len(rows)} cycles, {stops} stops'
                         + (f', {seen} times {expect!r}' if expect else ''))
    left = sorted({m for m in caught if m != before[0]}) if before else ['?']
    if left or len(caught) != n:
        raise SystemExit(f'FAIL: {n} cycles of {command[2]} left signal handlers behind: SigCgt '
                         f'{before[0] if before else "?"} before the core, {left} after a cycle')
    # Mappings as VmSize without the heap, whose size is its high-water mark
    # (glibc trims only its top): on RADV it grew ~5.8 MiB once, at the
    # first, second or third cycle as the driver's threads and glslang's
    # symbol tables happened to interleave, then stayed. What malloc holds
    # (mallinfo2, -1 without it) stands for the heap.
    first, last = rows[1], rows[-1]
    mapped = last[2] - last[4] - (first[2] - first[4])
    held = last[5] - first[5] if first[5] >= 0 and last[5] >= 0 else 0
    if (last[0] != first[0] or last[1] != first[1] or mapped > 4096 or held > 4096
            or last[3] - first[3] > 64 * 1024):
        raise SystemExit(f'FAIL: {n} cycles of {command[2]} left something behind: {first} then {last} '
                         '(threads, descriptors, VmSize KiB, VmRSS KiB, heap KiB, malloc KiB)')
    print(f'PASS: {n} cycles of {Path(command[2]).name} in one process, after each {last[0]} thread(s), '
          f'{last[1]} descriptor(s) and the signal handlers as before the core; mappings but the heap '
          f'{first[2] - first[4]} to {last[2] - last[4]} KiB, malloc {first[5]} to {last[5]} KiB, '
          f'VmRSS {first[3]} to {last[3]} KiB')


def xbe_of(image):
    """Extract default.xbe from a disc() image for use on the console disk."""
    return image.read_bytes()[34 * 2048:]


def run_without(command, markers, absent, env=None):
    """Like run(), but also reject any absent markers in the output."""
    result = subprocess.run([str(x) for x in command], env=env, capture_output=True, text=True, timeout=15)
    out = result.stdout + result.stderr
    missing = [m for m in markers if m not in out]
    found = [m for m in absent if m in out]
    if result.returncode or missing or found:
        print(out[-4000:])
        raise SystemExit(f'FAIL: {command}: exit {result.returncode}, missing {missing!r}, unexpected {found!r}')
    print(f'PASS: {Path(command[0]).name} {list(map(str, command[1:]))}: without {list(absent)!r}')


def run_counts(command, counts, env=None):
    """Like run(), with each marker in the output exactly as many times as counts says."""
    result = subprocess.run([str(x) for x in command], env=env, capture_output=True, text=True, timeout=15)
    out = result.stdout + result.stderr
    wrong = {m: out.count(m) for m, n in counts.items() if out.count(m) != n}
    if result.returncode or wrong:
        print(out[-4000:])
        raise SystemExit(f'FAIL: {command}: exit {result.returncode}, {wrong!r} times, not as in {counts!r}')
    print(f'PASS: {Path(command[0]).name} {list(map(str, command[1:]))}: {counts!r}')


def account(area, sector, gamertag, domain):
    """Write an online account to the config sector the app reads."""
    base = sector * 512
    area[base + 0x18:base + 0x28] = gamertag.encode().ljust(16, b'\0')
    area[base + 0x30:base + 0x44] = domain.encode().ljust(20, b'\0')


def dynamic_exports(path):
    """The defined global and weak symbols in an ELF64 object's .dynsym."""
    data = Path(path).read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 2:
        raise SystemExit(f'FAIL: {path}: not ELF64')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3a)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    names = []
    for name, kind, _, _, offset, size, link, _, _, entsize in sections:
        if kind != 11:          # SHT_DYNSYM
            continue
        strtab = sections[link]
        for i in range(1, size // entsize):
            st_name, st_info, _, st_shndx = struct.unpack_from('<IBBH', data, offset + i * entsize)
            if st_shndx == 0 or (st_info >> 4) not in (1, 2):   # undefined, or not global/weak
                continue
            start = strtab[4] + st_name
            names.append(data[start:data.index(b'\0', start)].decode())
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('harness', type=Path)
    parser.add_argument('core', type=Path)
    parser.add_argument('--audio-default', choices=('fast', 'dsp'), default='dsp',
                        help='Expected build default (Fast when built without the DSP JIT)')
    args = parser.parse_args()
    for name in ('NC_APU_DSP', 'NC_APU_DSP_JIT'):
        os.environ.pop(name, None)
    fast_processing = 'processing: fast dry mix (GP interpreter, EP interpreter)'
    full_processing = ('processing: full DSP (GP JIT, EP JIT)' if args.audio_default == 'dsp' else
                       'processing: full DSP (GP interpreter, EP interpreter)')
    # PortareOS's mode (HAKUX_LR_END_PROCESS) for the runs, whatever the build:
    # the stop's watchdog, and the way before M2 (HAKUX_LR_STOP=0).
    os.environ['HAKUX_LR_END_PROCESS'] = '1'
    old = dict(os.environ, HAKUX_LR_STOP='0')
    exports = dynamic_exports(args.core)
    stray = [n for n in exports if not n.startswith('retro_')]
    if stray or len(exports) < 25:
        raise SystemExit(f'FAIL: {args.core.name} exports {len(exports)} symbols; not libretro: {stray[:10]}')
    print(f'PASS: {args.core.name} exports only retro_* ({len(exports)})')
    run([args.harness, args.core], 0, ('core: hakuX', 'no game: supported',
                                       'extensions iso|chd, need_fullpath 1, block_extract 1',
                                       'av: 59.940060 fps, 48000 Hz', 'serialize size: 0', 'unloaded without a game',
                                       'option: hakux_render_scale (Render Scale), default 2',
                                       'option: hakux_crop_overscan (Crop Overscan), default enabled',
                                       'option: hakux_screen_format (Screen Format), default normal',
                                       'option: hakux_language (Language), default auto',
                                       'option: hakux_video_region (Video Region), default auto',
                                       'option: hakux_rumble (Rumble), default disabled',
                                       'option: hakux_communicator (Xbox Live Communicator), default disabled',
                                       'option: hakux_mic_sensitivity (Communicator Microphone Sensitivity), '
                                       'default medium',
                                       'option: hakux_audio_resampler (Audio Resampler), default sinc',
                                       f'option: hakux_audio_processing (Audio Processing), default {args.audio_default}',
                                       'option: hakux_frame_pace (Frame Pacing), default disabled'))
    with tempfile.TemporaryDirectory(prefix='hakux-libretro-') as folder:
        root = Path(folder)
        (root / 'saves').mkdir()
        (root / 'invalid.iso').write_bytes(b'not an Xbox disc')
        run([args.harness, args.core, root / 'missing.iso', 60, root / 'saves'], 3, 'load: refused')
        disc(root / 'halt.iso', b'\xf4')
        run([args.harness, args.core, root / 'halt.iso', 600, root / 'saves'], 0,
            ('guest executed HLT', 'the core asked to shut down', 'unload: the core stopped in'))
        run([args.harness, args.core, root / 'halt.iso', 600, root / 'saves'], 0,
            ('guest executed HLT', 'the core asked to shut down', 'the core does not stop (HAKUX_LR_STOP=0); '
             'ending the process'), old)
        if not (root / 'saves' / 'hdd' / 'Partition0').is_dir():
            raise SystemExit('FAIL: no console disk in the save directory')
        print('PASS: the console disk is in the save directory')
        # CHD images boot as the ISO does: one written here without
        # compression, chdman's (chd/: 0.289 createdvd of halt.iso padded to
        # whole sectors, its default codecs and zstd), and one laid out as a
        # full disc dump. A CD image is refused, and one cut short fails on
        # its last hunk, the XBE's.
        image = (root / 'halt.iso').read_bytes()
        chd(root / 'halt.chd', image)
        run([args.harness, args.core, root / 'halt.chd', 600, root / 'saves'], 0,
            ('disc: CHD v5, 73744 bytes in 19 hunks of 4096 (none, none, none, none)', 'guest executed HLT',
             'unload: the core stopped in'))
        for name, codecs in (('halt.chd', 'lzma, zlib, huff, flac'), ('halt-zstd.chd', 'zstd, none, none, none')):
            run([args.harness, args.core, Path(__file__).parent / 'chd' / name, 600, root / 'saves'], 0,
                (f'disc: CHD v5, 75776 bytes in 19 hunks of 4096 ({codecs})', 'guest executed HLT'))
        chd(root / 'dump.chd', image, lead=0x18300000)   # a full XGD1 dump's layout
        run([args.harness, args.core, root / 'dump.chd', 600, root / 'saves'], 0,
            ('disc: XDVDFS at +0x18300000', 'guest executed HLT'))
        chd(root / 'cd.chd', image, cd=True)
        run([args.harness, args.core, root / 'cd.chd', 600, root / 'saves'], 0,
            ('disc: CHD not read: a CD image', 'core: boot failed'))
        (root / 'short.chd').write_bytes((root / 'halt.chd').read_bytes()[:-4096])
        run([args.harness, args.core, root / 'short.chd', 600, root / 'saves'], 0,
            ('disc: CHD hunk 18: read error', 'core: boot failed'))
        # The way before M2: the frontend (here the harness) starts again in
        # its place and boots the XBE the launch data page names.
        disc(root / 'relaunch.iso', RELAUNCH, imports=(49, 164))
        run([args.harness, args.core, root / 'relaunch.iso', 600, root / 'saves'], 0,
            ('relaunch into \\Device\\CdRom0\\default.xbe', 'relaunch: starting RetroArch again',
             'relaunch: \\Device\\CdRom0\\default.xbe', 'guest executed HLT', 'ending the process'), old)
        if (root / 'saves' / 'relaunch.bin').exists():
            raise SystemExit('FAIL: the relaunch left its launch data page')
        # The core options reach the console before it boots: the defaults,
        # with RetroArch in German (RETRO_LANGUAGE_GERMAN, XC_LANGUAGE 3),
        # then each set.
        disc(root / 'settings.iso', SETTINGS, imports=(24,))
        env = dict(os.environ, HARNESS_LANGUAGE='4')
        run([args.harness, args.core, root / 'settings.iso', 600, root / 'saves'], 0,
            ('options: render scale 2x, overscan cropped, screen normal, language 3 (auto), video region auto',
             'aspect 1.333',
             'ex: video flags 00400000, AV pack 0, AV region 00400100, language 3', 'guest executed HLT'), env)
        env['HARNESS_OPTIONS'] = ('hakux_render_scale=1;hakux_screen_format=widescreen;hakux_language=french;'
                                  'hakux_video_region=ntsc-j')
        run([args.harness, args.core, root / 'settings.iso', 600, root / 'saves'], 0,
            ('options: render scale 1x, overscan cropped, screen widescreen, language 4 (french), video region ntsc-j, '
             'rumble off',
             'aspect 1.778', 'ex: video flags 00410000, AV pack 0, AV region 00400200, language 4',
             'guest executed HLT'), env)
        # A value the core doesn't know is its default.
        env['HARNESS_OPTIONS'] = 'hakux_render_scale=9;hakux_video_region=pal;hakux_crop_overscan=disabled'
        run([args.harness, args.core, root / 'settings.iso', 600, root / 'saves'], 0,
            ('options: render scale 2x, overscan shown, screen normal, language 3 (auto), video region pal',
             'ex: video flags 00400000, AV pack 0, AV region 00800300, language 3'), env)
        # Changed during the game (the menu), they wait for the next start;
        # the crop, rumble and frame pacing don't. Rumble on asks RetroArch
        # for its interface, which the harness doesn't have.
        disc(root / 'spin.iso', b'\xeb\xfe')
        env = dict(os.environ, HARNESS_OPTIONS_LATER='hakux_render_scale=3;hakux_video_region=pal;'
                   'hakux_crop_overscan=disabled;hakux_rumble=enabled;hakux_frame_pace=2')
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            ('options: render scale 2x', 'frame pacing off', 'message: Render Scale, Video Region: restart the game to apply',
             'rumble: the frontend has none', 'options: frame pacing 2 back', 'ran 60 frames'), env)
        # Ports 1-4 are players 1-4, a controller on each by default. A port
        # set to None in RetroArch's menu pulls its controller out while the
        # game runs, and one set back plugs it in; the next boot (here a
        # reset) has them as they are.
        vk = dict(os.environ, HARNESS_VULKAN='1')
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            ('controller info: 4 ports, each: Xbox Controller (1), None (0)', 'ohci: 4 controller(s) (players 1234)',
             'frame 25: ports: 1 0 0 1', 'port 2: none', "player 2's controller pulled out",
             "player 3's controller pulled out", 'frame 35: ports: 1 1 0 1', "player 2's controller plugged in",
             'reset at frame 45', 'ohci: 3 controller(s) (players 124)', 'ran 60 frames'),
            dict(vk, HARNESS_PORTS_AT='25:1,0,0,1;35:1,1,0,1', HARNESS_RESET_AT='45'))
        # The Xbox Live Communicator: off by default; on, in player 1's
        # controller from the boot, with the sensitivity's noise gate, and
        # freed with the console at each stop.
        run_without([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
                    ('communicator off', 'ran 30 frames'), ('has a Communicator',))
        voice = dict(os.environ, HARNESS_OPTIONS='hakux_communicator=enabled;hakux_mic_sensitivity=high')
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('communicator on (noise gate -46)', "ohci: player 1's controller has a Communicator",
             'communicator 1: noise gate -46', 'ran 30 frames'), voice)
        cycles([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], voice, 5,
               "player 1's controller has a Communicator")
        # RetroArch sets the ports as the game starts to boot: before the
        # controllers are made or just after, the console has the same ones.
        run([args.harness, args.core, root / 'spin.iso', 40, root / 'saves'], 0,
            ('ports: 1 0 0 0', 'port 4: none', 'reset at frame 20', 'ohci: 1 controller(s) (players 1)',
             'ran 40 frames'),
            dict(vk, HARNESS_PORTS='1,0,0,0', HARNESS_RESET_AT='20'))
        # RetroArch's cheats, applied as the game loads, before the guest
        # boots (context_reset): the title halts once the byte is there and
        # spins without. A code the core can't read is logged once, though
        # RetroArch sends it again; a reset keeps the cheats, their misses
        # counted again.
        disc(root / 'cheat.iso',
             b'\x80\x3d\x00\x0e\x01\x00\x5a'   # cmp byte [0x10e00], 0x5a
             b'\x75\xf7'                       # jne back
             b'\xf4')                          # hlt
        run([args.harness, args.core, root / 'cheat.iso', 600, root / 'saves'], 0,
            ('cheats: 1 applied', 'guest executed HLT'), dict(vk, HARNESS_CHEATS='w8 0x10E00 0x5A'))
        run_without([args.harness, args.core, root / 'cheat.iso', 60, root / 'saves'], ('ran 60 frames',),
                    ('guest executed HLT',), vk)
        run_counts([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
                   {'frame 10: cheats: 2 applied': 1, '"w9 0x100 1": code 1: unknown code; it stays off': 1},
                   dict(vk, HARNESS_CHEATS='w8 0x100 1|w9 0x100 1', HARNESS_CHEATS_AT='10'))
        run_counts([args.harness, args.core, root / 'spin.iso', 50, root / 'saves'],
                   {'reset at frame 20': 1, 'reset: the core stopped in': 1,
                    "the frontend's cheat 0: an address isn't mapped (yet)": 2},
                   dict(vk, HARNESS_CHEATS='w8 0x100 1', HARNESS_RESET_AT='20'))
        # XBE patches (xbe-patch-v1 codes, a .cht's emulator-handled
        # entries): over the XBE as it boots when its CRC-32 and size match.
        # The title halts only with the memory cheat (its byte) and the patch
        # (its jmp $ made hlt) both on: the two kinds side by side.
        disc(root / 'xbepatch.iso',
             b'\x80\x3d\x00\x0e\x01\x00\x5a'   # cmp byte [0x10e00], 0x5a
             b'\x75\xf7'                       # jne back
             b'\xeb\xfe')                      # jmp $ (at file offset 0x1009)
        xbe = xbe_of(root / 'xbepatch.iso')

        def patch(crc=zlib.crc32(xbe), size=len(xbe), runs='+00001009:F4F4'):
            return f'xbe-patch-v1 {crc:08X}:{size:08X}{runs}'
        poke = 'w8 0x10E00 0x5A'
        xp = [args.harness, args.core, root / 'xbepatch.iso', 600, root / 'saves']
        run(xp, 0, ('XBE patch (cheat 1): 1 runs, 2 bytes over default.xbe', 'guest executed HLT'),
            dict(vk, HARNESS_CHEATS=f'{poke}|{patch()}'))
        xp60 = xp[:3] + [60] + xp[4:]
        run_without(xp60, ('ran 60 frames',), ('guest executed HLT',), dict(vk, HARNESS_CHEATS=poke))
        run_without(xp60, ('XBE patch (cheat 0)', 'ran 60 frames'), ('guest executed HLT',),
                    dict(vk, HARNESS_CHEATS=patch()))
        # another revision's (its CRC, its size), a malformed one, two for
        # the same XBE: it boots unpatched
        for codes, marker in ((patch(crc=zlib.crc32(xbe) ^ 1), 'none for default.xbe'),
                              (patch(size=len(xbe) + 0x1000), 'none for default.xbe'),
                              (patch(runs='+00000002:F4F4'), 'below offset 4'),
                              (patch(runs='+00001009:F4+00001009:F4'), 'not after run 1'),
                              (f'{patch()}|{patch(runs="+00001009:F4")}', 'cheats 1 and 2 both patch default.xbe')):
            run_without(xp60, (marker, 'ran 60 frames'), ('guest executed HLT', 'XBE patch (cheat'),
                        dict(vk, HARNESS_CHEATS=f'{poke}|{codes}'))
        # turned on while the game runs (after its boot read the patches:
        # the XBE loaded) it takes a restart (Quick Menu, Restart): told once,
        # then the restarted game is patched
        late = dict(vk, HARNESS_CHEATS_LATE='1', HARNESS_READY='stage S0 xbe loaded')
        run_counts(xp,
                   {'XBE patches changed since the boot': 1, 'XBE patch (cheat 1)': 1, 'guest executed HLT': 1},
                   dict(late, HARNESS_CHEATS=f'{poke}|{patch()}', HARNESS_CHEATS_AT='r1', HARNESS_RESET_AT='m5'))
        # on, off (the set the game booted with: nothing to tell), on again:
        # told each time it differs
        run_counts(xp[:3] + [300] + xp[4:], {'XBE patches changed since the boot': 2, 'cheats off': 1},
                   dict(late, HARNESS_CHEATS=patch(), HARNESS_CHEATS_AT='r1,m10', HARNESS_CHEATS_OFF_AT='m5'))
        # RetroArch numbers the cheats that are on: a memory cheat turned off
        # before the patch moves it from 1 to 0, the same patch, nothing to tell
        run_counts(xp[:3] + [300] + xp[4:],
                   {'XBE patches changed since the boot': 0, 'XBE patch (cheat 1)': 1, 'cheats: 1 applied': 1},
                   dict(vk, HARNESS_CHEATS=f'w8 0x10E04 1|{patch()}', HARNESS_CHEATS2=patch(),
                        HARNESS_READY='stage S0 xbe loaded', HARNESS_CHEATS2_AT='r5'))
        # M2: the core stops in the process and boots again: 100 games and 20
        # from a CHD, a CPU in a loop that never enters the kernel, the
        # renderer on a device of its own and on the frontend's, a reset, a
        # title's relaunch.
        m2 = dict(os.environ, HAKUX_HLE_MODE='off')
        cycles([args.harness, args.core, root / 'halt.iso', 600, root / 'saves'], m2, 100)
        cycles([args.harness, args.core, root / 'halt.chd', 600, root / 'saves'], m2, 20)
        cycles([args.harness, args.core, root / 'spin.iso', 20, root / 'saves'], m2, 20)
        cycles([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], os.environ, 20)
        cycles([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
               dict(os.environ, HARNESS_VULKAN='1'), 20)
        # Upstream's network and NIC threads also stop before the next boot;
        # failed backend starts and failed boots must release their resources.
        for network, marker in (
                ('net=lan lanport=43075 lanbcast=127.0.0.1', 'lan: started'),
                ('net=kai kai=127.0.0.1:43076', 'kai: started'),
                (f'net=nat {NAT_TEST}', 'nat: UDP 43080 forwarded')):
            cycles([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
                   dict(os.environ, NC_NET=network), 5, marker)
        # a backend that fails to start (LAN's port taken, without
        # SO_REUSEADDR here) releases what it made
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as busy:
            busy.bind(('0.0.0.0', 43079))
            cycles([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
                   dict(os.environ, NC_NET='net=lan lanport=43079 lanbcast=127.0.0.1'), 5, "lan: couldn't start")
        cycles([args.harness, args.core, root / 'halt.iso', 60, root / 'saves'],
               dict(os.environ, NC_NET='net=lan lanport=43075 lanbcast=127.0.0.1', NC_BOOT_FAIL_AT='disc'),
               5, 'boot: failing after disc')
        # The Network option: off by default (no cable); LAN on the local
        # network's broadcast, with the consoles lan-peers.txt in the save
        # directory names (comments, commas, spaces and lines).
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('network off', 'network off: the NIC has no link', 'console timing on', 'ran 30 frames'))
        peers = root / 'saves' / 'lan-peers.txt'
        peers.write_text('# other consoles\n127.0.0.1:43077, 10.0.0.5\n\n10.0.0.6  # over a VPN\n')
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('network lan', 'consoles named: 127.0.0.1:43077,10.0.0.5,10.0.0.6', 'lan: UDP port 3075',
             '3 peers named', 'lan: started', 'ran 30 frames'),
            dict(os.environ, HARNESS_OPTIONS='hakux_network=lan'))
        peers.unlink()
        # Xbox Live (Insignia): NAT with Insignia's DNS server, or the first
        # address insignia-dns.txt in the save directory names (an
        # insigniaDNS server; NAT gives the console one), never a word that
        # isn't one
        insignia = dict(os.environ, HARNESS_OPTIONS='hakux_network=insignia', NC_NET=NAT_TEST)
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('network insignia', 'Xbox Live through Insignia (NAT), DNS 46.101.64.175',
             'nat: libslirp 4.7.0, 10.0.2.0/24, DNS 46.101.64.175', "nat: UDP 43080 forwarded to the console's 3074",
             'nat: started', 'ran 30 frames'),
            insignia)
        dns = root / 'saves' / 'insignia-dns.txt'
        dns.write_text('# insigniaDNS at home\n192.168.1.20, 192.168.1.21\n192.168.1.22\n')
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('DNS 192.168.1.20 (insignia-dns.txt)', 'nat: libslirp 4.7.0, 10.0.2.0/24, DNS 192.168.1.20',
             'nat: started', 'ran 30 frames'),
            insignia)
        dns.write_text('insignia.example\n')
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('names no IPv4 address (insignia.example)', 'DNS 46.101.64.175', 'nat: started', 'ran 30 frames'),
            insignia)
        dns.unlink()
        # Log only Insignia accounts from the config area, as the app does.
        # If none exist, tell game users where to create one.
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('network: no Insignia account on this console', 'message: No Insignia account on this console yet',
             'ran 30 frames'), insignia)
        config = root / 'saves' / 'hdd' / 'raw-partition0.bin'
        before = config.read_bytes() if config.exists() else None
        area = bytearray((before or b'').ljust(16 * 512, b'\0'))
        account(area, 8, 'hakuX Tester', 'insignia.live')
        account(area, 9, 'Elsewhere', 'xboxlive.com')
        account(area, 10, 'Second', 'INSIGNIA.LIVE')
        config.write_bytes(area)
        run_without([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
                    ('network: Insignia accounts on this console: hakuX Tester, Second', 'ran 30 frames'),
                    ('No Insignia account', 'Elsewhere'), insignia)
        if before is None:
            config.unlink()
        else:
            config.write_bytes(before)
        # Start Core boots the dashboard from C: with no disc. If missing,
        # refuse to load and show where to install its files.
        c_drive = root / 'saves' / 'hdd' / 'Partition2'
        run([args.harness, args.core, '-', 60, root / 'saves'], 3,
            ("The Xbox dashboard isn't installed: copy its files (xboxdash.xbe and its folders) to", 'load: refused'))
        (c_drive / 'XBOXDASH.XBE').write_bytes(xbe_of(root / 'halt.iso'))     # case-insensitive lookup
        run([args.harness, args.core, '-', 600, root / 'saves'], 0,
            ('loaded the dashboard (no content)', 'message: Xbox Live in the dashboard needs Network set to Xbox Live '
             '(Insignia)', 'guest executed HLT', 'the core asked to shut down', 'unload: the core stopped in'))
        (c_drive / 'XBOXDASH.XBE').unlink()
        # Dashboard users need no prompt to create an Insignia account.
        (c_drive / 'xboxdash.xbe').write_bytes(xbe_of(root / 'spin.iso'))
        run_without([args.harness, args.core, '-', 30, root / 'saves'],
                    ('network: Xbox Live through Insignia', 'network: no Insignia account on this console',
                     'ran 30 frames'), ('No Insignia account', 'needs Network set'), insignia)
        # Reset, video driver rebuild and repeated loads reboot the dashboard
        # without opening a disc.
        vk = dict(os.environ, HARNESS_VULKAN='1')
        run([args.harness, args.core, '-', 60, root / 'saves'], 0,
            ('reset at frame 20', 'reset: the core stopped in', 'ran 60 frames'), dict(vk, HARNESS_RESET_AT='20'))
        run([args.harness, args.core, '-', 60, root / 'saves'], 0,
            ('video driver rebuilt at frame 20', 'message: RetroArch rebuilt its video driver: the dashboard starts '
             'again', 'ran 60 frames'), dict(vk, HARNESS_REINIT_AT='20'))
        cycles([args.harness, args.core, '-', 30, root / 'saves'], vk, 5, 'loaded the dashboard (no content)')
        # Switching between a game and the dashboard boots the correct XBE.
        # The game halts; the dashboard spins.
        run([args.harness, args.core, '-', 300, root / 'saves'], 0,
            ('loaded the dashboard (no content)', 'then: ', 'guest executed HLT', 'the core asked to shut down'),
            dict(os.environ, HARNESS_THEN=str(root / 'halt.iso')))
        run([args.harness, args.core, root / 'halt.iso', 300, root / 'saves'], 0,
            ('guest executed HLT', 'then: -', 'loaded the dashboard (no content)', 'ran 300 frames'),
            dict(os.environ, HARNESS_THEN='-'))
        # Round-trip through Xbox Live's pages (xodash\xonlinedash.xbe) to
        # the dashboard's main menu, both in process and via a pre-M2 restart.
        # A title requesting the main menu still closes.
        disc(root / 'hop.iso', DASH_HOP, imports=(49, 164))
        disc(root / 'back.iso', DASH_BACK, imports=(49, 164))
        (c_drive / 'xboxdash.xbe').write_bytes(xbe_of(root / 'hop.iso'))
        (c_drive / 'xodash').mkdir(exist_ok=True)
        (c_drive / 'xodash' / 'xonlinedash.xbe').write_bytes(xbe_of(root / 'back.iso'))
        hops = ('relaunch into \\Device\\Harddisk0\\Partition2\\xodash\\xonlinedash.xbe',
                'relaunch: \\Device\\Harddisk0\\Partition2\\xodash\\xonlinedash.xbe',
                'relaunch into \\Device\\Harddisk0\\Partition2\\xboxdash.xbe',
                'relaunch: \\Device\\Harddisk0\\Partition2\\xboxdash.xbe', 'guest executed HLT')
        run([args.harness, args.core, '-', 600, root / 'saves'], 0,
            hops + ("relaunch: the dashboard boots again in RetroArch's process", 'unload: the core stopped in'))
        # Restart on -L <core>, using environment variables for harness frames
        # and saves.
        restart = dict(old, HARNESS_FRAMES='600', HARNESS_SAVES=str(root / 'saves'))
        run([args.harness, args.core, '-', 600, root / 'saves'], 0,
            hops + ('relaunch: starting RetroArch again for the dashboard', 'ending the process'), restart)
        # Both menu-loaded and -L/--menu starts must restart on -L <core>,
        # keeping config and -s but dropping --menu. Omit HARNESS_SAVES so
        # losing -s cannot silently select the same console disk.
        cfg, saves = root / 'retroarch.cfg', root / 'saves'
        menu_env = {k: v for k, v in restart.items() if k != 'HARNESS_SAVES'}
        menu_env['HARNESS_MENU_CORE'] = str(args.core)
        for name, command in (('started from the menu', [args.harness, '-c', cfg, '-s', saves, '--menu']),
                              ('started on -L with --menu',
                               [args.harness, '-L', args.core, '-c', cfg, '-s', saves, '--menu'])):
            menu = subprocess.run([str(x) for x in command], env=menu_env, capture_output=True, text=True, timeout=15)
            out = menu.stdout + menu.stderr
            missing = [m for m in hops + (f'relaunch: RetroArch starts again with its config and -L '
                                          f'{args.core.resolve()}',
                                          'relaunch: starting RetroArch again for the dashboard',
                                          f'core from the command line: -L {args.core.resolve()}',
                                          'ending the process') if m not in out]
            starts = out.count('core from the command line: -L ') + out.count('core loaded from the menu: ')
            kept = (out.count(f'config: {cfg}'), out.count(f'save directory from the command line: -s {saves}'))
            if menu.returncode or missing or kept != (starts, starts) \
                    or out.count('option: --menu') != 1 or "can't start again" in out:
                print(out[-4000:])
                raise SystemExit(f'FAIL: the dashboard {name}: exit {menu.returncode}, missing {missing!r}, '
                                 f'{kept} (config, save directory) in {starts} starts, '
                                 f'{out.count("option: --menu")} times --menu')
            print(f'PASS: the dashboard {name} started again on -L and the core, its config and save directory '
                  'kept, without --menu')
        if (root / 'saves' / 'relaunch.bin').exists():
            raise SystemExit("FAIL: the dashboard's relaunch left its launch data page")
        run([args.harness, args.core, root / 'back.iso', 600, root / 'saves'], 0,
            ('title returned to the firmware (routine 2', 'the core asked to shut down'))
        print("PASS: the dashboard booted with no content, through its relaunches")
        # The audio resampler option reaches the APU before it starts: sinc by
        # default (NEON on ARM64; on x86-64 AVX2+FMA or SSE2, the CPU's),
        # linear when chosen
        sinc = 'resampler: sinc (32-tap Kaiser-windowed, ' + ('NEON)' if os.uname().machine == 'aarch64' else '')
        run_without([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'],
                    ('resampler sinc', sinc, 'ran 30 frames'), ('not in this build',))
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('resampler linear', 'resampler: linear', 'ran 30 frames'),
            dict(os.environ, HARNESS_OPTIONS='hakux_audio_resampler=linear'))
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            (f'audio processing {args.audio_default}',
             full_processing if args.audio_default == 'dsp' else fast_processing, 'ran 30 frames'))
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('audio processing fast', fast_processing, 'ran 30 frames'),
            dict(os.environ, HARNESS_OPTIONS='hakux_audio_processing=fast'))
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('audio processing dsp', 'processing: full DSP (GP interpreter, EP interpreter)', 'ran 30 frames'),
            dict(os.environ, HARNESS_OPTIONS='hakux_audio_processing=dsp', NC_APU_DSP_JIT='0'))
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            ('audio processing fast', full_processing, 'ran 30 frames'),
            dict(os.environ, HARNESS_OPTIONS='hakux_audio_processing=fast', NC_APU_DSP='1'))
        run([args.harness, args.core, root / 'spin.iso', 30, root / 'saves'], 0,
            (f'audio processing {args.audio_default}', fast_processing, 'ran 30 frames'),
            dict(os.environ, NC_APU_DSP='0'))
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            (full_processing, fast_processing,
             'reset at frame 40', 'ran 60 frames'),
            dict(os.environ, HARNESS_OPTIONS='hakux_audio_processing=dsp',
                 HARNESS_OPTIONS_LATER='hakux_audio_processing=fast', HARNESS_RESET_AT='40'))
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            ('reset at frame 20', 'reset: the core stopped in', 'ran 60 frames',
             'RetroArch destroyed the context: the core stopped in', 'stop: the front end stopped the console',
             'back end started'),
            dict(os.environ, HARNESS_VULKAN='1', HARNESS_RESET_AT='20'))
        # RetroArch rebuilding its video driver: the core stops off the old
        # device and boots the game again on the new one, and says so.
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            ('video driver rebuilt at frame 20', 'RetroArch destroyed the context: the core stopped in',
             'message: RetroArch rebuilt its video driver: the game starts again', 'ran 60 frames'),
            dict(os.environ, HARNESS_VULKAN='1', HARNESS_REINIT_AT='20'))
        # Check that reset and video driver rebuild clear compatibility caches.
        compat = dict(os.environ, HARNESS_VULKAN='1', HAKUX_HLE_CAPS='-vk13,-dynrender,-sync2,-eds1,-eds2')
        compat_line = 'compat: Vulkan below 1.3, render passes, 1.0 barriers and submits'
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            (compat_line, 'reset at frame 20', 'reset: the core stopped in', 'back end started',
             'ran 60 frames'),
            dict(compat, HARNESS_RESET_AT='20'))
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            (compat_line, 'video driver rebuilt at frame 20',
             'message: RetroArch rebuilt its video driver: the game starts again', 'ran 60 frames'),
            dict(compat, HARNESS_REINIT_AT='20'))
        run([args.harness, args.core, root / 'relaunch.iso', 600, root / 'saves'], 0,
            ('relaunch into \\Device\\CdRom0\\default.xbe', 'relaunch: the game boots again in RetroArch\'s process',
             'relaunch: the core stopped in', 'relaunch: \\Device\\CdRom0\\default.xbe', 'guest executed HLT',
             'unload: the core stopped in'))
        if (root / 'saves' / 'relaunch.bin').exists():
            raise SystemExit('FAIL: the relaunch in the process left its launch data page')
        run([args.harness, args.core, root / 'halt.iso', 600, root / 'saves'], 0,
            ('guest executed HLT', 'unload: the core stopped in', 'load again: loaded'),
            dict(os.environ, HARNESS_LOAD_AGAIN='1'))
        print('PASS: the relaunch booted the XBE its page named')
        # A boot that fails at any stage leaves nothing behind either: each
        # stage's failure five times in one process, the renderer's also on
        # the frontend's device.
        for stage in ('kernel', 'disc', 'xbe', 'cpu', 'thread', 'hle'):
            cycles([args.harness, args.core, root / 'halt.iso', 60, root / 'saves'],
                   dict(os.environ, NC_BOOT_FAIL_AT=stage), 5, f'boot: failing after {stage}')
        cycles([args.harness, args.core, root / 'halt.iso', 60, root / 'saves'],
               dict(os.environ, NC_BOOT_FAIL_AT='hle', HARNESS_VULKAN='1'), 5, 'boot: failing after hle')
        # A title update boots from its host file (the disc XBE's, title
        # ffff0001): its descriptor is closed with the boot, run or failed.
        update = root / 'saves' / 'hdd' / 'Partition1' / 'TDATA' / 'ffff0001' / '$u' / 'default.xbe'
        update.parent.mkdir(parents=True, exist_ok=True)
        update.write_bytes((root / 'halt.iso').read_bytes()[34 * 2048:])
        cycles([args.harness, args.core, root / 'halt.iso', 60, root / 'saves'], os.environ, 5, 'title update: ')
        cycles([args.harness, args.core, root / 'halt.iso', 60, root / 'saves'],
               dict(os.environ, NC_BOOT_FAIL_AT='xbe'), 5, 'boot: failing after xbe')
        update.unlink()
        # PortareOS's build: a stop that outlasts its watchdog (here any: 1 ms)
        # ends the process. Outside it the stop waits, and HAKUX_LR_STOP=0
        # changes nothing.
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            ('ran 60 frames', 'unload: the core did not stop in 1 ms', 'ending the process'),
            dict(os.environ, HAKUX_LR_STOP_TIMEOUT_MS='1'))
        run([args.harness, args.core, root / 'spin.iso', 60, root / 'saves'], 0,
            ('ran 60 frames', 'unload: the core stopped in', 'unloaded'),
            dict(old, HAKUX_LR_END_PROCESS='0', HAKUX_LR_STOP_TIMEOUT_MS='1'))


if __name__ == '__main__':
    main()

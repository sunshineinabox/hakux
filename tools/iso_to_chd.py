#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert an Xbox ISO to an optimized XISO-based DVD CHD.

Requires the xdvdfs and chdman commands on PATH. The source is never modified.
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path, help="Xbox ISO or Redump ISO")
    parser.add_argument("-o", "--output", type=Path, help="CHD path (default: input name with .chd)")
    parser.add_argument("--keep-xiso", action="store_true", help="also save the intermediate .xiso.iso")
    args = parser.parse_args(argv)

    try:
        source = args.iso.expanduser().resolve(strict=True)
    except OSError as exc:
        parser.error(str(exc))
    if not source.is_file() or source.suffix.lower() != ".iso":
        parser.error("input must be an existing .iso file")

    output = args.output.expanduser().resolve() if args.output else source.with_suffix(".chd")
    if output.suffix.lower() != ".chd":
        parser.error("output must end in .chd")
    xiso_output = output.with_suffix(".xiso.iso")
    if not output.parent.is_dir():
        parser.error("output directory does not exist")
    if output.exists() or (args.keep_xiso and xiso_output.exists()):
        parser.error("output already exists; refusing to overwrite it")
    if args.keep_xiso and xiso_output == source:
        parser.error("intermediate XISO would overwrite the input")

    xdvdfs, chdman = shutil.which("xdvdfs"), shutil.which("chdman")
    if not xdvdfs or not chdman:
        parser.error("install xdvdfs-cli (xdvdfs) and chdman first")

    try:
        with tempfile.TemporaryDirectory(prefix=".xiso-to-chd-", dir=output.parent) as tmp:
            xiso = Path(tmp) / "game.xiso.iso"
            chd = Path(tmp) / "game.chd"
            subprocess.run([xdvdfs, "pack", str(source), str(xiso)], check=True)
            if not xiso.is_file() or not xiso.stat().st_size:
                raise RuntimeError("xdvdfs did not create an XISO")
            subprocess.run([chdman, "createdvd", "-i", str(xiso), "-o", str(chd)], check=True)
            if not chd.is_file() or not chd.stat().st_size:
                raise RuntimeError("chdman did not create a CHD")
            if args.keep_xiso:
                xiso.replace(xiso_output)
            chd.replace(output)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        parser.exit(1, f"conversion failed: {exc}\n")

    print(f"Created {output}")
    if args.keep_xiso:
        print(f"Kept {xiso_output}")


if __name__ == "__main__":
    main()

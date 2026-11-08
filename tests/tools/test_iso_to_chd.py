# SPDX-License-Identifier: GPL-2.0-or-later
"""CLI tests for the ISO -> XISO -> CHD wrapper, using fake converters."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "iso_to_chd.py"
FAKE_TOOL = """#!/usr/bin/env python3
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
if os.environ.get("FAIL_TOOL") == name:
    sys.exit(3)
args = sys.argv[1:]
if name == "xdvdfs":
    assert args[0] == "pack"
    source, target = Path(args[1]), Path(args[2])
    target.write_bytes(b"XISO:" + source.read_bytes())
else:
    assert args[0] == "createdvd"
    source = Path(args[args.index("-i") + 1])
    target = Path(args[args.index("-o") + 1])
    target.write_bytes(b"CHD:" + source.read_bytes())
"""


class ConverterTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "Game with spaces.iso"
        self.src.write_bytes(b"REDUMP")
        bindir = self.root / "bin"
        bindir.mkdir()
        for name in ("xdvdfs", "chdman"):
            exe = bindir / name
            exe.write_text(FAKE_TOOL)
            exe.chmod(0o755)
        self.env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"])

    def convert(self, *args, fail=None):
        env = dict(self.env)
        if fail:
            env["FAIL_TOOL"] = fail
        return subprocess.run([sys.executable, str(SCRIPT), str(self.src), *args],
                              capture_output=True, text=True, env=env)

    def test_conversion_removes_intermediate(self):
        result = self.convert()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.src.read_bytes(), b"REDUMP")
        self.assertEqual(self.src.with_suffix(".chd").read_bytes(), b"CHD:XISO:REDUMP")
        self.assertFalse(self.root.joinpath("Game with spaces.xiso.iso").exists())

    def test_keep_xiso(self):
        result = self.convert("--keep-xiso")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.root.joinpath("Game with spaces.xiso.iso").read_bytes(),
                         b"XISO:REDUMP")

    def test_custom_output(self):
        output = self.root / "New name.chd"
        result = self.convert("-o", str(output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), b"CHD:XISO:REDUMP")

    def test_refuses_existing_file(self):
        output = self.src.with_suffix(".chd")
        output.write_bytes(b"KEEP")
        result = self.convert()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b"KEEP")

    def test_failure_cleans_temporary_files(self):
        for tool in ("xdvdfs", "chdman"):
            with self.subTest(tool=tool):
                result = self.convert(fail=tool)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.src.with_suffix(".chd").exists())
                self.assertFalse(any(self.root.glob(".xiso-to-chd-*")))
                self.assertEqual(self.src.read_bytes(), b"REDUMP")

    def test_rejects_overwriting_input_as_intermediate(self):
        self.src = self.root / "Game.xiso.iso"
        self.src.write_bytes(b"XISO")
        result = self.convert("--keep-xiso", "-o", str(self.root / "Game.chd"))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.src.read_bytes(), b"XISO")


if __name__ == "__main__":
    unittest.main()

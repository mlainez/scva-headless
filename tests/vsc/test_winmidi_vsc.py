#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# scva-winmidi32.exe with --engine vsc, under wine. wine's midiIn has no
# inputs to listen on, so this goes as far as it can: the engine opens - the
# Windows 98 build's own loader mapping vsc.sys - before the tool stops for
# want of a MIDI input. Playing is a manual check (docs/MANUAL-CHECKS.md).
# Needs SCVA_TEST_VSC_DIR and wine.
import os
import shutil
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
BUILD = os.path.join(ROOT, "build")
WINMIDI = os.path.join(BUILD, "scva-winmidi32.exe")
sys.path.insert(0, os.path.join(ROOT, "tests", "fake"))
from harness import case, skip, summary  # noqa: E402


def winpath(p):
    return subprocess.run(["winepath", "-w", p], capture_output=True, text=True).stdout.strip()


def winmidi(*args):
    env = dict(os.environ, WINEDEBUG="-all",
               WINEPREFIX=os.environ.get("SCVA_TEST_WINEPREFIX",
                                         os.path.join(BUILD, "test", "wineprefix")))
    p = subprocess.run(["wine", WINMIDI] + list(args), capture_output=True, timeout=60, env=env)
    return p.returncode, (p.stdout + p.stderr).decode(errors="replace").replace("\r", "")


def opens_the_engine_then_wants_an_input(c):
    status, text = winmidi("--engine", "vsc", "--core", winpath(os.path.join(DATA, "vsc.sys")),
                           "--midi-in", "no-such-input-here")
    c.expect(status == 1, f"exit 1, got {status}: {text[-300:]}")
    c.expect("no MIDI input" in text, f"stopped at the MIDI input, past the engine: {text[-300:]}")


def a_missing_engine_file_is_named(c):
    status, text = winmidi("--engine", "vsc", "--core", "C:\\\\nowhere\\\\vsc.sys")
    c.expect(status == 1, f"exit 1, got {status}")
    c.expect("vsc.sys" in text, f"the file named: {text[-300:]}")


def a_rate_vsc_lacks(c):
    status, text = winmidi("--engine", "vsc", "--core", winpath(os.path.join(DATA, "vsc.sys")),
                           "--rate", "48000")
    c.expect(status == 2, f"exit 2, got {status}")
    c.expect("22050" in text and "44100" in text, f"the rates it has: {text[-300:]}")


if __name__ == "__main__":
    DATA = os.environ.get("SCVA_TEST_VSC_DIR")
    if not DATA:
        print("skip test_winmidi_vsc (set SCVA_TEST_VSC_DIR)")
        sys.exit(0)
    if not os.path.exists(WINMIDI) or not shutil.which("wine"):
        skip("scva-winmidi32.exe with --engine vsc", "not built, or no wine")
        sys.exit(summary())
    for name, fn in (
        ("opens VSC's engine, then wants a MIDI input", opens_the_engine_then_wants_an_input),
        ("names a missing engine file", a_missing_engine_file_is_named),
        ("refuses a rate VSC lacks, naming the ones it has", a_rate_vsc_lacks),
    ):
        case(f"scva-winmidi32.exe --engine vsc: {name}", fn)
    sys.exit(summary())

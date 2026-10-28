#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# The VSC backend against virtual_sc's vsc-play, the oracle. Every render
# must come out byte for byte the same as vsc-play's:
#
#   - the backend driven as vsc-play drives its engine: every song, at each
#     of VSC's rates, with each of its tone sets;
#   - the same, asking for frames in other amounts than the engine's block,
#     at each rate (each has its own block size), with one tone set;
#   - scva-render32 and scva-render32.exe (under wine) with --engine vsc:
#     every song at each rate, each rate with another tone set.
#
#   SCVA_TEST_VSC_DIR    vsc.sys and the vscNN.dat files (required)
#   SCVA_TEST_VSC_PLAY   the vsc-play binary (required)
#   SCVA_TEST_CORPUS     the songs to play, *.mid (required)
#   SCVA_TEST_VSC_QUICK  set: one rate, one set and one call size, for a fast check
import filecmp
import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
BUILD = os.path.join(ROOT, "build")
DRIVE = os.path.join(BUILD, "test", "vsc_drive")
RATES = ("11025", "22050", "44100")
SETS = {"55": "vsc55.dat", "88": "vsc88.dat", "88pro": "vsc88pro.dat"}
# One frame, less than any block, more than one, many at once.
CHUNKS = ("1", "64", "100", "333", "4096")
# The renderers: one tone set per rate, so all three sets are heard.
RENDER_RUNS = (("11025", "55"), ("22050", "88pro"), ("44100", "88"))


def need(var):
    v = os.environ.get(var)
    if not v:
        print(f"skip test-vsc (set {var})")
        sys.exit(0)
    return v


def winpath(p):
    return subprocess.run(["winepath", "-w", p], capture_output=True, text=True).stdout.strip()


def backend(data, song, rate, name, got, chunk=None):
    return [DRIVE, "--core", os.path.join(data, "vsc.sys"),
            "--tones", os.path.join(data, SETS[name]), "--rate", rate,
            "--midi", song, "--out", got] + (["--chunk", chunk] if chunk else [])


def renderer(exe, data, song, rate, name, got):
    common = ["--engine", "vsc", "--set", name, "--rate", rate, "--bits", "16",
              "--tail", "2", "--flat-out"]
    if exe.endswith(".exe"):
        return ["wine", os.path.join(BUILD, exe), "--core", winpath(os.path.join(data, "vsc.sys")),
                "--midi", winpath(song), "--out", winpath(got)] + common
    return [os.path.join(BUILD, exe), "--core", os.path.join(data, "vsc.sys"),
            "--midi", song, "--out", got] + common


def main():
    data = need("SCVA_TEST_VSC_DIR")
    oracle = need("SCVA_TEST_VSC_PLAY")
    songs = sorted(glob.glob(os.path.join(need("SCVA_TEST_CORPUS"), "*.mid")))
    rates, sets, chunks, render_runs = RATES, SETS, CHUNKS, RENDER_RUNS
    if os.environ.get("SCVA_TEST_VSC_QUICK"):
        rates, sets, chunks = ("22050",), {"88pro": SETS["88pro"]}, ("100",)
        render_runs = (("22050", "88pro"),)
    if not songs:
        print("FAIL no songs in the corpus")
        return 1
    os.environ.setdefault("WINEDEBUG", "-all")
    os.environ.setdefault("WINEPREFIX", os.path.join(BUILD, "test", "wineprefix"))
    renderers = [r for r in ("scva-render32", "scva-render32.exe")
                 if os.path.exists(os.path.join(BUILD, r))
                 and (not r.endswith(".exe") or shutil.which("wine"))]

    # (what, song, rate, set, command for the output at got)
    runs = []
    for song in songs:
        for rate in rates:
            for name in sets:
                runs.append(("backend", song, rate, name,
                             lambda got, s=song, r=rate, n=name: backend(data, s, r, n, got)))
            for chunk in chunks:
                runs.append((f"backend, {chunk} frames a call", song, rate, "88pro",
                             lambda got, s=song, r=rate, c=chunk: backend(data, s, r, "88pro", got, c)))
        for rate, name in render_runs:
            for exe in renderers:
                runs.append((exe, song, rate, name,
                             lambda got, s=song, r=rate, n=name, e=exe: renderer(e, data, s, r, n, got)))

    tmp = tempfile.mkdtemp()
    ref, got = os.path.join(tmp, "ref.wav"), os.path.join(tmp, "got.wav")
    failed = passed = 0
    rendered = None
    for what, song, rate, name, command in runs:
        label = f"{os.path.basename(song)} {rate} Hz {name}: {what}"
        if rendered != (song, rate, name):
            a = subprocess.run([oracle, "-q", "--data", data, "--set", name, "--rate", rate,
                                "--out", ref, song], capture_output=True, text=True)
            if a.returncode:
                print(f"FAIL {label}: vsc-play {a.returncode} {a.stderr.strip()[-200:]}")
                failed += 1
                rendered = None
                continue
            rendered = (song, rate, name)
        if os.path.exists(got):
            os.remove(got)
        b = subprocess.run(command(got), capture_output=True, text=True)
        if b.returncode:
            print(f"FAIL {label}: exit {b.returncode} {b.stderr.strip()[-200:]}")
            failed += 1
        elif not filecmp.cmp(ref, got, shallow=False):
            print(f"FAIL {label}: the renders differ "
                  f"({os.path.getsize(ref)} and {os.path.getsize(got)} bytes)")
            failed += 1
        else:
            print(f"ok   {label}")
            passed += 1
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

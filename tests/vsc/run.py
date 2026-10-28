#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# The VSC backend against virtual_sc's vsc-play, the oracle: every song in
# the corpus, at each of VSC's rates and with each of its tone sets, must
# render to the same bytes through both. Then again with the backend asked
# for frames in other amounts than its own block, at each rate (each has its
# own block size), with one tone set.
#
#   SCVA_TEST_VSC_DIR    vsc.sys and the vscNN.dat files (required)
#   SCVA_TEST_VSC_PLAY   the vsc-play binary (required)
#   SCVA_TEST_CORPUS     the songs to play, *.mid (required)
#   SCVA_TEST_VSC_QUICK  set: one rate, one set and one call size, for a fast check
import filecmp
import glob
import os
import subprocess
import sys
import tempfile

DRIVE = os.path.join(os.path.dirname(__file__), "..", "..", "build", "test", "vsc_drive")
RATES = ("11025", "22050", "44100")
SETS = {"55": "vsc55.dat", "88": "vsc88.dat", "88pro": "vsc88pro.dat"}
# One frame, less than any block, more than one, many at once.
CHUNKS = ("1", "64", "100", "333", "4096")


def need(var):
    v = os.environ.get(var)
    if not v:
        print(f"skip test-vsc (set {var})")
        sys.exit(0)
    return v


def main():
    data = need("SCVA_TEST_VSC_DIR")
    oracle = need("SCVA_TEST_VSC_PLAY")
    songs = sorted(glob.glob(os.path.join(need("SCVA_TEST_CORPUS"), "*.mid")))
    rates, sets, chunks = RATES, SETS, CHUNKS
    if os.environ.get("SCVA_TEST_VSC_QUICK"):
        rates, sets, chunks = ("22050",), {"88pro": SETS["88pro"]}, ("100",)
    if not songs:
        print("FAIL no songs in the corpus")
        return 1
    tmp = tempfile.mkdtemp()
    failed = passed = 0
    runs = [(song, rate, name, dat, None) for song in songs for rate in rates
            for name, dat in sets.items()]
    runs += [(song, rate, "88pro", SETS["88pro"], chunk) for song in songs for rate in rates
             for chunk in chunks]
    for song, rate, name, dat, chunk in runs:
        ref, got = os.path.join(tmp, "ref.wav"), os.path.join(tmp, "got.wav")
        what = f"{os.path.basename(song)} {rate} Hz {name}"
        if chunk:
            what += f", {chunk} frames a call"
        a = subprocess.run([oracle, "-q", "--data", data, "--set", name, "--rate", rate,
                            "--out", ref, song], capture_output=True, text=True)
        b = subprocess.run([DRIVE, "--core", os.path.join(data, "vsc.sys"),
                            "--tones", os.path.join(data, dat), "--rate", rate,
                            "--midi", song, "--out", got]
                           + (["--chunk", chunk] if chunk else []),
                           capture_output=True, text=True)
        if a.returncode or b.returncode:
            print(f"FAIL {what}: vsc-play {a.returncode} {a.stderr.strip()[-200:]}, "
                  f"backend {b.returncode} {b.stderr.strip()[-200:]}")
            failed += 1
        elif not filecmp.cmp(ref, got, shallow=False):
            print(f"FAIL {what}: the renders differ "
                  f"({os.path.getsize(ref)} and {os.path.getsize(got)} bytes)")
            failed += 1
        else:
            print(f"ok   {what}")
            passed += 1
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

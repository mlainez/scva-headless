#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# How much work the tools do around the engine, in instructions retired, so
# that a change which makes them heavier is caught even when no one would
# hear it. Each measurement must stay within BUDGET of its baseline.
#
#   SCVA_TEST_DLL_DIR   the user's own SCCore.dll and name files (required)
#   SCVA_TEST_CORPUS    .mid files; the first two are measured if present
#   --capture           record the baseline for this core and this machine
#   --budget PCT        allowed growth, default 0.25 (counts do not drift)
#
# Baselines are per core and per processor model, since glibc and the core's
# own SSE3 shim pick code by what the CPU offers.
import hashlib
import os
import re
import subprocess
import sys
import tempfile

BUILD = os.environ.get("BUILD", "build")
DLL_DIR = os.environ.get("SCVA_TEST_DLL_DIR")
CORPUS = os.environ.get("SCVA_TEST_CORPUS")
CAPTURE = "--capture" in sys.argv
BUDGET = float(sys.argv[sys.argv.index("--budget") + 1]) if "--budget" in sys.argv else 0.25
ICOUNT = os.path.join(BUILD, "test", "icount")


def count(cmd, env=None, stdin=None):
    p = subprocess.run([ICOUNT] + cmd, capture_output=True, env=env, input=stdin, timeout=900)
    m = re.search(rb"instructions (\d+)", p.stderr)
    if p.returncode != 0 or not m:
        raise RuntimeError(f"{' '.join(cmd)} failed ({p.returncode}): {p.stderr[-300:]!r}")
    return int(m.group(1))


def cpu_model():
    for line in open("/proc/cpuinfo"):
        if line.startswith("model name"):
            return re.sub(r"[^A-Za-z0-9]+", "-", line.split(":", 1)[1].strip()).strip("-")
    return "unknown-cpu"


def measurements():
    core = os.path.join(DLL_DIR, "SCCore.dll")
    songs = []
    if CORPUS and os.path.isdir(CORPUS):
        songs = [os.path.join(CORPUS, n) for n in sorted(os.listdir(CORPUS)) if n.lower().endswith(".mid")][:2]
    if not songs:
        songs = [os.path.join(os.environ["SCVA_TEST_MIDI"], "timing.mid")]
    out = os.path.join(tempfile.mkdtemp(), "out.wav")
    for s in songs:
        name = os.path.basename(s)
        yield f"scva-render {name}", lambda s=s: count(
            [os.path.join(BUILD, "scva-render"), "--core", core, "--midi", s, "--out", out, "--flat-out"])
    # The LV2 plugin rendering 20 s in host-sized blocks, a note sounding.
    script = "midi 0 90 3c 64\n" + "run 512\n" * (20 * 48000 // 512) + "quit\n"
    env = dict(os.environ, SCVA_DLL_DIR=DLL_DIR)
    yield "scva.lv2 20 s in 512-frame runs", lambda: count(
        [os.path.join(BUILD, "test", "lv2_host64"), os.path.join(BUILD, "scva.lv2")],
        env=env, stdin=script.encode())


def main():
    if not DLL_DIR or not os.path.exists(os.path.join(DLL_DIR, "SCCore.dll")):
        print("skip performance tests (set SCVA_TEST_DLL_DIR)")
        return 0
    with open(os.path.join(DLL_DIR, "SCCore.dll"), "rb") as f:
        core_id = hashlib.sha1(f.read()).hexdigest()[:12]
    path = os.path.join("tests", "golden", f"perf-{core_id}-{cpu_model()}.txt")
    base = {}
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.startswith("#"):
                k, v = line.rsplit(" ", 1)
                base[k] = int(v)
    got, failed = {}, 0
    for name, measure in measurements():
        n = measure()
        got[name] = n
        if CAPTURE:
            print(f"     {name}: {n}")
            continue
        if name not in base:
            print(f"skip {name}: no baseline for this core and CPU (run with --capture)")
            continue
        change = 100.0 * (n - base[name]) / base[name]
        verdict = "ok  " if change <= BUDGET else "FAIL"
        failed += verdict == "FAIL"
        print(f"{verdict} {name}: {change:+.2f}% instructions (budget +{BUDGET:g}%)")
    if CAPTURE:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write("# user-space instructions retired, baseline\n")
            for k, v in got.items():
                f.write(f"{k} {v}\n")
        print(f"wrote {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

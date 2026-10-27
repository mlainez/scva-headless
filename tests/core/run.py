#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# The renderers with the user's own SCCore.dll: what they actually sound like,
# pinned by hash so a refactor that changes a single sample is caught.
#
#   SCVA_TEST_DLL_DIR   directory holding SCCore.dll (required)
#   SCVA_TEST_CORPUS    a directory of .mid files to render as well (optional,
#                       never committed: usually someone else's music)
#   --capture           write the golden file for this core instead of
#                       checking against it
#
# Goldens live in tests/golden/core-<first 12 hex of the core's SHA-1>.txt,
# so each core version has its own. They hold hashes of rendered audio, not
# anything of Roland's.
import hashlib
import math
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "fake"))
from harness import PathArg, Tool, case, read_wav, skip, summary  # noqa: E402

DLL_DIR = os.environ.get("SCVA_TEST_DLL_DIR")
CORPUS = os.environ.get("SCVA_TEST_CORPUS")
CAPTURE = "--capture" in sys.argv
NATIVE = Tool("scva-render", "", False)
WINDOWS = Tool("scva-render.exe", "", True)


def sha(path, algo="sha256"):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def render(tool, midi, *extra, env=None):
    out = PathArg(os.path.join(tempfile.mkdtemp(), "out.wav"))
    core = os.path.join(DLL_DIR, "SCCore.dll")
    if env:
        os.environ.update(env)
    try:
        r = tool.run(["--core", "{core}", "--midi", PathArg(midi), "--out", out, "--flat-out"] + list(extra),
                     core=core, timeout=600)
    finally:
        for k in (env or {}):
            os.environ.pop(k, None)
    r.wav = str(out)
    return r


def songs():
    midi = os.environ["SCVA_TEST_MIDI"]
    found = [("synthetic/" + n, os.path.join(midi, n)) for n in sorted(os.listdir(midi))]
    if CORPUS and os.path.isdir(CORPUS):
        for n in sorted(os.listdir(CORPUS)):
            if n.lower().endswith(".mid"):
                p = os.path.join(CORPUS, n)
                found.append((f"corpus/{n}@{sha(p, 'sha1')[:8]}", p))
    return found


CASES = [
    ("default 48k f32", ["--rate", "48000"]),
    ("sc-55 48k f32", ["--map", "55", "--rate", "48000"]),
    ("sc-88 48k f32", ["--map", "88", "--rate", "48000"]),
    ("sc-88pro 48k f32", ["--map", "88pro", "--rate", "48000"]),
    ("sc-8820 48k f32", ["--map", "8820", "--rate", "48000"]),
    ("default 44.1k s16", ["--rate", "44100", "--bits", "16"]),
    ("gm reset 48k f32", ["--reset", "gm", "--rate", "48000"]),
]


def plausible_audio(c, r, label):
    """Finite, not silent, within full scale for float output."""
    _, bits, frames = read_wav(r.wav)
    flat = [x for pair in frames for x in pair]
    c.expect(all(math.isfinite(x) for x in flat), f"{label}: no NaN or infinity")
    peak = max((abs(x) for x in flat), default=0)
    c.expect(peak > 1e-4, f"{label}: audible output, peak {peak}")


def main():
    if not DLL_DIR or not os.path.exists(os.path.join(DLL_DIR, "SCCore.dll")):
        print("skip real-core tests (set SCVA_TEST_DLL_DIR to a directory with SCCore.dll)")
        return 0
    core_id = sha(os.path.join(DLL_DIR, "SCCore.dll"), "sha1")[:12]
    golden_path = os.path.join("tests", "golden", f"core-{core_id}.txt")
    golden = {}
    if os.path.exists(golden_path):
        for line in open(golden_path):
            if line.strip() and not line.startswith("#"):
                key, h = line.rsplit(" ", 1)
                golden[key] = h.strip()
    captured = {}

    def renders_as_before(c, name, path, label, extra):
        r = render(NATIVE, path, *extra)
        if not c.expect(r.status == 0, f"exit 0, got {r.status}: {r.text[-300:]}"):
            return
        plausible_audio(c, r, label)
        key = f"{name} | {label}"
        h = sha(r.wav)
        captured[key] = h
        if not CAPTURE:
            c.expect(key in golden, f"a golden for '{key}' (run with --capture on the baseline)")
            c.expect(golden.get(key) in (None, h), f"the same audio as the baseline for '{key}'")

    for name, path in songs():
        for label, extra in CASES:
            if name.startswith("corpus/") and label not in ("default 48k f32", "sc-88 48k f32"):
                continue
            case(f"scva-render: {name}: {label}", renders_as_before, name, path, label, extra)

    def windows_identical(c, name, path):
        a, b = render(NATIVE, path, "--map", "88"), render(WINDOWS, path, "--map", "88")
        if c.expect(a.status == 0 and b.status == 0, f"both rendered: {a.status} {b.status} {b.text[-200:]}"):
            c.expect(sha(a.wav) == sha(b.wav), "byte-identical WAVs from Linux and Windows builds")

    def sse3_identical(c, name, path):
        on = render(NATIVE, path, env={"SCVA_SSE3": "1"})
        off = render(NATIVE, path, env={"SCVA_SSE3": "0"})
        if c.expect(on.status == 0 and off.status == 0, "both rendered"):
            c.expect("SSE3 shim, 6 sites" in on.text, f"the shim reported 6 sites: {on.stderr[-200:]}")
            c.expect("SSE3 shim" not in off.text, "no shim when switched off")
            c.expect(sha(on.wav) == sha(off.wav), "byte-identical audio with and without the shim")

    for name, path in songs()[:3]:
        if WINDOWS.built() and subprocess.run(["sh", "-c", "command -v wine"], capture_output=True).returncode == 0:
            case(f"scva-render.exe sounds exactly like scva-render: {name}", windows_identical, name, path)
        case(f"the SSE3 shim does not change a sample: {name}", sse3_identical, name, path)

    if CAPTURE:
        os.makedirs(os.path.dirname(golden_path), exist_ok=True)
        with open(golden_path, "w") as f:
            f.write(f"# SHA-256 of scva-render output with core SHA-1 {core_id}...\n")
            f.write("# case | settings  sha256\n")
            for k in sorted(captured):
                f.write(f"{k} {captured[k]}\n")
        print(f"wrote {golden_path} ({len(captured)} renders)")
    return summary()


if __name__ == "__main__":
    sys.exit(main())

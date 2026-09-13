#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Look at a float32 WAV the way this project needs to, and compare two.

Everything is reported PER CHANNEL, and the summary names the worse of the
two rather than the first: a defect can live in one channel only.

  wavcmp.py stats a.wav [b.wav]      per-channel health, and a diff if given two
  wavcmp.py head a.wav [n]           the first n frames as numbers
"""
import struct
import sys


def read_wav(path):
    """Return (rate, [left, right]) of float32 samples. Chunk-walks rather
    than assuming the data chunk is at offset 44."""
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file")
    i, rate, bits, ch, fmt, data = 12, None, None, None, None, None
    while i + 8 <= len(b):
        cid, sz = b[i:i + 4], struct.unpack("<I", b[i + 4:i + 8])[0]
        body = b[i + 8:i + 8 + sz]
        if cid == b"fmt ":
            fmt, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            data = body
        i += 8 + sz + (sz & 1)
    if data is None or rate is None:
        raise SystemExit(f"{path}: no fmt/data chunk")
    if fmt != 3 or bits != 32:
        raise SystemExit(f"{path}: expected float32 (fmt 3, 32 bit), got fmt {fmt}, {bits} bit")
    n = len(data) // 4
    flat = struct.unpack("<%df" % n, data[:n * 4])
    return rate, [list(flat[c::ch]) for c in range(ch)]


def nonfinite(x):
    return x != x or x in (float("inf"), float("-inf"))


def stats(name, ch):
    """Per-channel health. A 'glitch' is a one-sample jump far larger than the
    signal's own typical step: the neighbours track, then one sample leaps and
    resyncs."""
    out = []
    for c, s in enumerate(ch):
        finite = [x for x in s if not nonfinite(x)]
        bad = len(s) - len(finite)
        peak = max((abs(x) for x in finite), default=0.0)
        steps = [abs(s[i] - s[i - 1]) for i in range(1, len(s))
                 if not nonfinite(s[i]) and not nonfinite(s[i - 1])]
        steps_sorted = sorted(steps)
        med = steps_sorted[len(steps_sorted) // 2] if steps_sorted else 0.0
        # threshold: well above the median step, and an absolute floor so a
        # near-silent passage does not make every ripple a glitch
        thr = max(med * 50.0, peak * 0.05, 1e-6)
        glitches = sum(1 for d in steps if d > thr)
        out.append((peak, bad, glitches, med, thr))
        print(f"{name} ch{c}: peak {peak:.6f}  nonfinite {bad}  "
              f"glitches {glitches}  median step {med:.3e}  thr {thr:.3e}")
    worst = max(g for _, _, g, _, _ in out)
    worstbad = max(b for _, b, _, _, _ in out)
    print(f"{name} WORST-CHANNEL: glitches {worst}, nonfinite {worstbad}")
    return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mode, path = sys.argv[1], sys.argv[2]
    rate, ch = read_wav(path)
    if mode == "head":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        print(f"{path}: {rate} Hz, {len(ch)} ch, {len(ch[0])} frames")
        for i in range(min(n, len(ch[0]))):
            print("  [%6d] " % i + "  ".join("%14.9g" % c[i] for c in ch))
    elif mode == "stats":
        print(f"{path}: {rate} Hz, {len(ch)} ch, {len(ch[0])} frames")
        stats("A", ch)
        if len(sys.argv) > 3:
            b_path = sys.argv[3]
            rate_b, ch_b = read_wav(b_path)
            print(f"{b_path}: {rate_b} Hz, {len(ch_b)} ch, {len(ch_b[0])} frames")
            stats("B", ch_b)
            n = min(len(ch[0]), len(ch_b[0]))
            print(f"--- comparing first {n} frames ---")
            for c in range(min(len(ch), len(ch_b))):
                diffs = [abs(ch[c][i] - ch_b[c][i]) for i in range(n)]
                first = next((i for i, d in enumerate(diffs) if d > 0), None)
                print(f"ch{c}: max|A-B| {max(diffs, default=0):.6g}  "
                      f"first differing frame {first}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()

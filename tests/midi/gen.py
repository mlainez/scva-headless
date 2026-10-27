#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# Writes the small Standard MIDI Files the engine-level tests play. Each one
# exercises a behaviour; none is music.
#
#   gen.py OUTDIR
import os
import struct
import sys

PPQ = 480                       # at 120 bpm a tick is 1/960 s


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.insert(0, 0x80 | (v & 0x7F))
        v >>= 7
    return bytes(out)


def track(events):
    """events: (absolute tick, raw bytes) in order."""
    body, last = b"", 0
    for tick, raw in events:
        body += vlq(tick - last) + raw
        last = tick
    body += vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(body)) + body


def smf(fmt, tracks):
    return b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), PPQ) + b"".join(tracks)


def sysex(body):
    """An F0 event: the SMF stores it without its F0."""
    return b"\xf0" + vlq(len(body)) + body


TEMPO_120 = b"\xff\x51\x03\x07\xa1\x20"
GS_REVERB_HALL2 = bytes([0x41, 0x10, 0x42, 0x12, 0x40, 0x01, 0x30, 0x04, 0x0B, 0xF7])

SONGS = {
    # A note, a controller and a SysEx at known times: 0, 480 (0.5 s) and
    # 960 (1 s), ending at 1440 (1.5 s).
    "timing.mid": smf(0, [track([
        (0, TEMPO_120),
        (0, bytes([0x90, 60, 100])),
        (480, bytes([0xB0, 7, 90])),
        (960, sysex(GS_REVERB_HALL2)),
        (1440, bytes([0x80, 60, 0])),
    ])]),

    # Program changes, and a CC32 written in the file itself.
    "programs.mid": smf(0, [track([
        (0, TEMPO_120),
        (0, bytes([0xC0, 5])),
        (0, bytes([0xB1, 32, 2])),
        (0, bytes([0xC1, 40])),
        (10, bytes([0x91, 64, 100])),
        (480, bytes([0x81, 64, 0])),
    ])]),

    # Two ports: port B named by FF 21. Each port plays a note on channel 1;
    # a SysEx on port A must reach both engines.
    "twoport.mid": smf(1, [
        track([(0, TEMPO_120), (0, bytes([0x90, 60, 100])),
               (240, sysex(GS_REVERB_HALL2)), (480, bytes([0x80, 60, 0]))]),
        track([(0, b"\xff\x21\x01\x01"), (0, bytes([0xC0, 33])),
               (0, bytes([0x90, 67, 100])), (480, bytes([0x80, 67, 0]))]),
    ]),
}

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, data in SONGS.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)

#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# The LV2 plugin in a minimal host (tests/plugins/lv2_host.c) against the fake
# core: what instantiating needs, what its Tone Map, Instrument and Drum Kit
# ports send, how host MIDI reaches the engine, and what comes out. The host
# runs the plugin on a different thread from the one that instantiated it.
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness
from harness import GS_RESET, case, parse_trace, skip, summary

BUILD = harness.BUILD
VARIANTS = [("scva.lv2", "lv2_host64", "linux64"), ("scva32.lv2", "lv2_host32", "linux32")]

# Every map has its own page: two tones (program 1 with a variation on bank
# 8, program 2) and two kits (Standard, Room), except SC-55 with Standard only.
TNF = "MODULENAME=SC-8820\r\n" + "".join(
    f"PAGE={m}\t{m}Map\r\n\t0\t8\r\n1\tPiano {m}\tPiano {m}w\r\n2\tBright {m}\t---\r\n"
    for m in range(5))
DRF = "".join(
    f"PAGE={m}\t{m}Map\r\n\t0\r\n1\tStandard {m}\r\n" + ("" if m == 1 else f"9\tRoom {m}\r\n")
    for m in range(5))


def install(names=True, table=False, core="linux64"):
    """A directory holding a fake core and (optionally) name files."""
    d = tempfile.mkdtemp()
    shutil.copy(os.path.join(harness.FAKE, core, "SCCore.dll"), os.path.join(d, "SCCore.dll"))
    if names:
        open(os.path.join(d, "SCVSC.tnf"), "w").write(TNF)
        open(os.path.join(d, "SCVSC.drf"), "w").write(DRF)
    if table:
        open(os.path.join(d, "scva-tones.txt"), "w").write("t 2 0 0 My Piano\nd 2 0 0 My Kit\n")
    return d


class Session:
    def __init__(self, variant, script, **kw):
        bundle, host, core = variant
        d = install(core=core, **kw)
        trace = os.path.join(d, "trace")
        fd = os.open(trace, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        env = dict(os.environ, SCVA_DLL_DIR=d)
        p = subprocess.run([os.path.join(BUILD, "test", host), os.path.join(BUILD, bundle)],
                           input=script.encode(), capture_output=True, env=env, timeout=60,
                           pass_fds=(3,), preexec_fn=lambda: os.dup2(fd, 3))
        os.close(fd)
        self.status = p.returncode
        self.out = p.stdout.decode(errors="replace")
        self.err = p.stderr.decode(errors="replace")
        self.calls = parse_trace(open(trace, errors="replace").read())
        self.runs = []
        lines = self.out.splitlines()
        for i, l in enumerate(lines):
            if l.startswith("ran "):
                self.runs.append([int(x) for x in lines[i + 1].split()[1:]])

    @property
    def instantiated(self):
        return "instantiated" in self.out.splitlines()

    def midi(self):
        return [c.midi for c in self.calls if c.midi]

    def at(self, frame):
        """Short messages that reached the engine when it had rendered
        exactly this many frames: what a run starting there sent first."""
        return [x.midi for x in self.calls if x.midi and x.frame == frame]


def ok(c, s):
    return c.expect(s.instantiated and s.status == 0,
                    f"instantiated and exit 0, got {s.status}: {(s.out + s.err)[-300:]}")


def needs_names(c, v):
    s = Session(v, "quit\n", names=False)
    c.expect(not s.instantiated, "no instance without name files")
    c.expect("SCVSC" in s.err, f"a message naming the files it needs: {s.err!r}")


def accepts_own_table(c, v):
    s = Session(v, "run 64\nquit\n", names=False, table=True)
    ok(c, s)


def renders_on_another_thread(c, v):
    s = Session(v, "run 256\nrun 256\nquit\n")
    if not ok(c, s):
        return
    want = [n % 64 for n in range(512)]
    got = s.runs[0] + s.runs[1] if len(s.runs) == 2 else []
    c.expect(got == want, "the engine's samples, uninterrupted across runs")
    c.expect(any(x.sysex == GS_RESET for x in s.calls), "the engine reset to GS")


def map_port(c, v):
    """A Tone Map change moves every part, re-sending its program, once."""
    s = Session(v, "port map 2\nrun 64\nrun 64\nport map 3\nrun 64\nquit\n")
    if not ok(c, s):
        return
    for start, value in ((0, 2), (128, 3)):
        sent = s.at(start)
        for ch in range(16):
            c.expect((0xb0 | ch, 32, value) in sent, f"run at {start}: channel {ch + 1} on map {value}")
            c.expect(any(m[0] == 0xc0 | ch for m in sent), f"run at {start}: channel {ch + 1}'s program re-sent")
    c.expect(not s.at(64), f"nothing sent while the map stays the same: {s.at(64)}")


def instrument_port(c, v):
    """The Instrument port picks a tone of the current map for part 1, sent
    when it changes and not otherwise."""
    s = Session(v, "port map 2\nport inst 1\nrun 64\nrun 64\nport inst 2\nrun 64\nquit\n")
    if not ok(c, s):
        return
    # part 1's patch messages: its CC32, CC0 and program change
    part1 = lambda msgs: [m for m in msgs if m[0] == 0xc0 or (m[0] == 0xb0 and m[1] in (0, 32))]
    # map 2's tones in file order: Piano (0/0), Piano w (8/0), Bright (0/1)
    c.expect(part1(s.at(0))[-3:] == [(0xb0, 32, 2), (0xb0, 0, 8), (0xc0, 0, 0)],
             f"position 1: Piano 2w, bank 8 program 1, got {part1(s.at(0))[-3:]}")
    c.expect(not part1(s.at(64)), f"nothing re-sent when unchanged: {part1(s.at(64))}")
    c.expect(part1(s.at(128)) == [(0xb0, 32, 2), (0xb0, 0, 0), (0xc0, 1, 0)],
             f"position 2: Bright 2, bank 0 program 2, got {part1(s.at(128))}")


def kit_port(c, v):
    """The Drum Kit port is a GS program for part 10; a kit the map does not
    have falls back to Standard."""
    s = Session(v, "port map 2\nport kit 8\nrun 64\nport kit 99\nrun 64\nport map 1\nport kit 8\nrun 64\nquit\n")
    if not ok(c, s):
        return
    kit = lambda frame: [m for m in s.at(frame) if m[0] in (0xb9, 0xc9)][-3:]
    c.expect(kit(0) == [(0xb9, 32, 2), (0xb9, 0, 0), (0xc9, 8, 0)], f"Room on map 2: {kit(0)}")
    c.expect(kit(64) == [(0xb9, 32, 2), (0xb9, 0, 0), (0xc9, 0, 0)], f"Standard for 99: {kit(64)}")
    c.expect(kit(128) == [(0xb9, 32, 1), (0xb9, 0, 0), (0xc9, 0, 0)],
             f"Standard on SC-55, which has no Room: {kit(128)}")


def sample_accurate(c, v):
    """Host MIDI lands on its frame: the block is split there."""
    s = Session(v, "run 64\nmidi 100 90 3c 64\nmidi 200 80 3c 00\nrun 256\nquit\n")
    if not ok(c, s):
        return
    on = [x for x in s.calls if x.midi == (0x90, 0x3c, 0x64)]
    off = [x for x in s.calls if x.midi == (0x80, 0x3c, 0x00)]
    c.expect(on and on[0].frame == 64 + 100, f"the note-on at frame 164, got {on}")
    c.expect(off and off[0].frame == 64 + 200, f"the note-off at frame 264, got {off}")
    c.expect(s.runs and s.runs[1] == [n % 64 for n in range(64, 320)], "the output unbroken by the split")


def host_programs(c, v):
    """On a chosen map a host program change gets that map first; on the
    default map it goes as it is."""
    s = Session(v, "port map 3\nrun 64\nmidi 0 c2 10\nrun 64\nquit\n")
    if not ok(c, s):
        return
    i = next(i for i, x in enumerate(s.calls) if x.midi == (0xc2, 0x10, 0))
    c.expect(s.calls[i - 1].midi == (0xb2, 32, 3), f"CC32=3 before it, got {s.calls[i - 1]}")
    s = Session(v, "run 64\nmidi 0 c2 10\nrun 64\nquit\n")
    i = next(i for i, x in enumerate(s.calls) if x.midi == (0xc2, 0x10, 0))
    c.expect(not (s.calls[i - 1].midi and s.calls[i - 1].midi[1] == 32), f"nothing before it: {s.calls[i - 1]}")


def host_sysex(c, v):
    s = Session(v, "midi 10 f0 41 10 42 12 40 01 30 04 0b f7\nrun 64\nquit\n")
    if ok(c, s):
        c.expect(any(x.sysex == "f0 41 10 42 12 40 01 30 04 0b f7" for x in s.calls), "the SysEx, whole")


def cleanup(c, v):
    s = Session(v, "run 64\nquit\n")
    if not ok(c, s):
        return
    fns = [x.fn for x in s.calls]
    c.expect("TG_deactivate" in fns and "TG_terminate" not in fns, "deactivated, never terminated")
    c.expect("cleaned up" in s.out, "the host outlives the plugin")


def main():
    for v in VARIANTS:
        bundle, host, core = v
        if not os.path.exists(os.path.join(BUILD, bundle, "scva.so")):
            skip(bundle, "not built")
            continue
        if not os.path.exists(os.path.join(BUILD, "test", host)):
            skip(f"{bundle} host", "not built (make lv2-hosts)")
            continue
        for name, fn in (
            ("refuses to start without the name files, and says which", needs_names),
            ("starts with a scva-tones.txt instead", accepts_own_table),
            ("renders the engine's output on the host's audio thread", renders_on_another_thread),
            ("moves every part when Tone Map changes, once", map_port),
            ("sends the Instrument's tone to part 1 when it changes", instrument_port),
            ("sends the Drum Kit to part 10, or Standard if the map lacks it", kit_port),
            ("splits the block at each MIDI event's frame", sample_accurate),
            ("puts the map before a host program change, except on Default", host_programs),
            ("passes host SysEx through whole", host_sysex),
            ("deactivates the engine on cleanup, never terminates", cleanup),
        ):
            case(f"{bundle}: {name}", fn, v)
    return summary()


if __name__ == "__main__":
    sys.exit(main())

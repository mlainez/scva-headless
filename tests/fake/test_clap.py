#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# The CLAP plugin, in all four builds, in a minimal host (tests/plugins/
# clap_host.c) against the fake core: its parameters and their texts, map
# changes arriving while processing, the instrument and kit choices, host
# MIDI, preset discovery and loading, drum key names, and the General MIDI
# fallback. The host processes on a different thread from the main one.
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import harness
from harness import GS_RESET, case, parse_trace, skip, summary, wine_env, winpath

BUILD = harness.BUILD
VARIANTS = [
    ("scva.clap", "clap_host64", "linux64", False),
    ("scva32.clap", "clap_host32", "linux32", False),
    ("scva-win64.clap", "clap_host_win64.exe", "win64", True),
    ("scva-win32.clap", "clap_host_win32.exe", "win32", True),
]

# Every map: Piano (bank 0 program 1), Piano w (bank 8 program 1), Bright
# (bank 0 program 2); kits Standard (program 1) and Room (program 9). SC-55
# lacks Piano w and Room, so its lists differ in length and order and a map
# change has to look a patch up rather than keep its position. Room on 8820
# names two keys.
TNF = "MODULENAME=SC-8820\r\n" + "".join(
    f"PAGE={m}\t{m}Map\r\n\t0\t8\r\n1\tPiano {m}\t" + ("---" if m == 1 else f"Piano {m}w") +
    f"\r\n2\tBright {m}\t---\r\n"
    for m in range(5))
DRF = "".join(
    f"PAGE={m}\t{m}Map\r\n\t0\r\n1\tStandard {m}\r\n" + ("" if m == 1 else f"9\tRoom {m}\r\n")
    for m in range(5))
DRK = "PAGE=4\t8820Map\r\n\t0\t8\r\n36\tKick\tRoom Kick\r\n38\tSnare\tRoom Snare\r\n"


class Session:
    def __init__(self, variant, script, names=True):
        clap, host, core, windows = variant
        d = tempfile.mkdtemp()
        shutil.copy(os.path.join(harness.FAKE, core, "SCCore.dll"), os.path.join(d, "SCCore.dll"))
        if names:
            open(os.path.join(d, "SCVSC.tnf"), "w").write(TNF)
            open(os.path.join(d, "SCVSC.drf"), "w").write(DRF)
            open(os.path.join(d, "SCVSC.drk"), "w").write(DRK)
        trace = os.path.join(d, "trace")
        host_path = os.path.abspath(os.path.join(BUILD, "test", host))
        plugin = os.path.abspath(os.path.join(BUILD, clap))
        script = "\n".join(l.strip() for l in script.strip().splitlines()) + "\nquit\n"
        if windows:
            env = wine_env()
            env["SCVA_DLL_DIR"] = winpath(d, env)
            env["SCVA_FAKE_LOG"] = winpath(trace, env)
            p = subprocess.run(["wine", host_path, winpath(plugin, env)], input=script.encode(),
                               capture_output=True, env=env, timeout=120)
        else:
            env = dict(os.environ, SCVA_DLL_DIR=d)
            fd = os.open(trace, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            p = subprocess.run([host_path, plugin], input=script.encode(), capture_output=True,
                               env=env, timeout=120, pass_fds=(3,),
                               preexec_fn=lambda: os.dup2(fd, 3))
            os.close(fd)
        self.status = p.returncode
        self.out = p.stdout.decode(errors="replace").replace("\r\n", "\n")
        self.err = p.stderr.decode(errors="replace")
        self.lines = self.out.splitlines()
        self.calls = parse_trace(open(trace, errors="replace").read()) if os.path.exists(trace) else []

    def first(self, prefix):
        return next((l[len(prefix):].strip() for l in self.lines if l.startswith(prefix)), None)

    def all(self, prefix):
        return [l[len(prefix):].strip() for l in self.lines if l.startswith(prefix)]

    def at(self, frame):
        return [x.midi for x in self.calls if x.midi and x.frame == frame]

    def outputs(self):
        """Left channel of each process call, in order."""
        res, take = [], False
        for l in self.lines:
            if l.startswith("processed "):
                take = True
            elif take and l.startswith("L"):
                res.append([int(x) for x in l.split()[1:]])
                take = False
        return res


def ok(c, s):
    return c.expect(s.status == 0 and "activated" in s.lines and "destroyed" in s.lines,
                    f"activated, ran and destroyed, got {s.status}: {(s.out + s.err)[-300:]}")


def parameters(c, v):
    s = Session(v, "params")
    if not ok(c, s):
        return
    c.expect(s.all("param ") == ["0 Tone Map|0|4|0", "1 Instrument|0|2|0", "2 Drum Kit|0|1|0"],
             f"Tone Map 0-4, Instrument and Drum Kit over the longest lists, got {s.all('param ')}")


def parameter_texts(c, v):
    s = Session(v, """text 0 2
                      text 1 1
                      text 2 1
                      fromtext 0 sc-88pro
                      fromtext 1 002 Bright 4
                      fromtext 1 nothing like it""")
    if not ok(c, s):
        return
    c.expect(s.all("text ") == ["0 SC-88", "1 001-008 Piano 4w", "2 009 Room 4"],
             f"map and patch labels (Default lists SC-8820's patches), got {s.all('text ')}")
    c.expect(s.all("fromtext ") == ["0 3", "1 2", "1 -"], f"texts read back, got {s.all('fromtext ')}")


def renders_on_audio_thread(c, v):
    s = Session(v, "process 128\nprocess 128")
    if not ok(c, s):
        return
    outs = s.outputs()
    c.expect(len(outs) == 2 and outs[0] + outs[1] == [n % 64 for n in range(256)],
             "the engine's samples, uninterrupted across calls")
    c.expect(any(x.sysex == GS_RESET for x in s.calls), "the engine reset to GS")


def map_change_while_processing(c, v):
    """A Tone Map change sent with notes in one process call applies to the
    whole block, before its notes (91de49c)."""
    s = Session(v, """process 64
                      set 0 2
                      midi 0 90 3c 64
                      process 64""")
    if not ok(c, s):
        return
    sent = s.at(64)
    note = sent.index((0x90, 0x3c, 0x64)) if (0x90, 0x3c, 0x64) in sent else None
    c.expect(note is not None, f"the note reached the engine at frame 64: {sent}")
    if note is None:
        return
    for ch in range(16):
        c.expect((0xb0 | ch, 32, 2) in sent[:note], f"channel {ch + 1} on map 2 before the note")


def map_change_carries_choices(c, v):
    """On a map change Instrument and Drum Kit move to the same patch in the
    new map, the host is told their new values, and asked to rescan."""
    s = Session(v, """set 1 2
                      set 2 1
                      process 64
                      set 0 1
                      process 64
                      callback""")
    if not ok(c, s):
        return
    outs = s.all("out ")
    c.expect("1 1" in outs, f"Instrument moved from position 2 to Bright 1's position 1, got {outs}")
    c.expect("2 0" in outs, f"Drum Kit reported at Standard, SC-55 having no Room, got {outs}")
    c.expect("callback" in s.lines and "rescan 3" in s.lines,
             f"a main-thread callback asking for values and texts to be rescanned, got {s.lines[-8:]}")


def instrument_and_kit(c, v):
    """Instrument picks part 1's tone, Drum Kit part 10's kit, each sent when
    it changes; a new kit renames the drum keys."""
    s = Session(v, """set 1 2
                      process 64
                      process 64
                      set 2 1
                      process 64
                      callback
                      notenames""")
    if not ok(c, s):
        return
    part1 = [m for m in s.at(0) if m[0] in (0xb0, 0xc0)][-3:]
    c.expect(part1 == [(0xb0, 32, 4), (0xb0, 0, 0), (0xc0, 1, 0)], f"part 1 on Bright 4, got {part1}")
    c.expect(not [m for m in s.at(64) if m[0] in (0xb0, 0xc0)], "nothing re-sent when unchanged")
    kit = [m for m in s.at(128) if m[0] in (0xb9, 0xc9)][-3:]
    c.expect(kit == [(0xb9, 32, 4), (0xb9, 0, 0), (0xc9, 8, 0)], f"part 10 on Room 4, got {kit}")
    c.expect("notenames changed" in s.lines, "the host told the drum key names changed")
    c.expect(s.all("note ") == ["36 9 Room Kick", "38 9 Room Snare"], f"Room's keys, got {s.all('note ')}")


def host_midi(c, v):
    s = Session(v, """process 64
                      midi 100 90 3c 64
                      sysex 150 f0 41 10 42 12 40 01 30 04 0b f7
                      midi 200 c2 05
                      process 256""")
    if not ok(c, s):
        return
    on = [x for x in s.calls if x.midi == (0x90, 0x3c, 0x64)]
    syx = [x for x in s.calls if x.sysex == "f0 41 10 42 12 40 01 30 04 0b f7"]
    c.expect(on and on[0].frame == 164, f"the note at frame 164, got {on}")
    c.expect(syx and syx[0].frame == 214, f"the SysEx, whole, at frame 214, got {syx}")
    pc = [i for i, x in enumerate(s.calls) if x.midi == (0xc2, 5, 0)]
    c.expect(pc and not (s.calls[pc[0] - 1].midi and s.calls[pc[0] - 1].midi[1] == 32),
             "on the default map, a host program change goes as it is")
    outs = s.outputs()
    c.expect(len(outs) == 2 and outs[1] == [n % 64 for n in range(64, 320)], "output unbroken by the splits")


def preset_discovery(c, v):
    s = Session(v, "presets")
    if not ok(c, s):
        return
    presets = s.all("preset ")
    c.expect(any("kind 0" in l or "kind 1" in l for l in s.all("location ")), "one plugin-provided location")
    c.expect(len(presets) == 11 + 7, f"every map's tones then its kits (11 + 7), got {len(presets)}")
    c.expect(presets[:3] == ["SC-55 001 Piano 1|1.0.0.t|instrument",
                             "SC-55 002 Bright 1|1.0.1.t|instrument",
                             "SC-88 001 Piano 2|2.0.0.t|instrument"], f"SC-55's tones first, got {presets[:3]}")
    c.expect(presets[-1] == "SC-8820 009 Room 4|4.0.8.d|drum", f"SC-8820's Room last, got {presets[-1:]}")


def preset_load(c, v):
    s = Session(v, """process 64
                      load 2.8.0.t
                      process 64
                      load 3.0.8.d
                      process 64
                      load 9.0.0.t""")
    if not ok(c, s):
        return
    c.expect(s.all("load ") == ["2.8.0.t ok", "3.0.8.d ok", "9.0.0.t refused"], f"loads, got {s.all('load ')}")
    tone = [m for m in s.at(64) if m[0] in (0xb0, 0xc0)]
    kit = [m for m in s.at(128) if m[0] in (0xb9, 0xc9)]
    c.expect(tone == [(0xb0, 32, 2), (0xb0, 0, 8), (0xc0, 0, 0)], f"part 1 on SC-88 Piano 2w, got {tone}")
    c.expect(kit == [(0xb9, 32, 3), (0xb9, 0, 0), (0xc9, 8, 0)], f"part 10 on SC-88Pro Room, got {kit}")
    c.expect("notenames changed" in s.lines, "a drum preset renames the keys")


def preset_before_first_process(c, v):
    """Known issue, reported rather than pinned: a preset loaded before the
    first process() is sent, then replaced in the same call by the
    Instrument parameter's own choice. A host restoring a session can do
    exactly that. This prints a note while it holds, and another once fixed."""
    s = Session(v, "load 2.8.0.t\nprocess 64")
    if not ok(c, s):
        return
    last = [m for m in s.at(0) if m[0] in (0xb0, 0xc0)][-3:]
    if last == [(0xb0, 32, 2), (0xb0, 0, 8), (0xc0, 0, 0)]:
        print("     note: fixed - remove the known-issue entry for presets before the first process()")
    else:
        print("     known issue: a preset loaded before the first process() is overridden by Instrument")


def gm_fallback(c, v):
    """Without name files the plugin still plays, and offers General MIDI."""
    s = Session(v, "params\npresets\nprocess 64", names=False)
    if not ok(c, s):
        return
    c.expect("1 Instrument|0|127|0" in s.all("param "), f"128 instruments, got {s.all('param ')}")
    presets = s.all("preset ")
    c.expect(len(presets) == 129, f"129 presets, got {len(presets)}")
    c.expect(presets and presets[0] == "001 Acoustic Grand Piano|0.0.0.t|instrument", f"got {presets[:1]}")


def stop(c, v):
    s = Session(v, "process 64")
    if not ok(c, s):
        return
    fns = [x.fn for x in s.calls]
    c.expect("TG_deactivate" in fns and "TG_terminate" not in fns, "deactivated, never terminated")


def main():
    for v in VARIANTS:
        clap, host = v[0], v[1]
        if not os.path.exists(os.path.join(BUILD, clap)):
            skip(clap, "not built")
            continue
        if not os.path.exists(os.path.join(BUILD, "test", host)):
            skip(f"{clap} host", "not built (make plugin-hosts)")
            continue
        for name, fn in (
            ("offers Tone Map, Instrument and Drum Kit", parameters),
            ("names parameter values and reads them back", parameter_texts),
            ("renders the engine's output on the host's audio thread", renders_on_audio_thread),
            ("applies a map change before the notes of the same block", map_change_while_processing),
            ("carries Instrument and Drum Kit across a map change", map_change_carries_choices),
            ("sends Instrument to part 1 and Drum Kit to part 10, with key names", instrument_and_kit),
            ("plays host MIDI on its frame and SysEx whole", host_midi),
            ("lists every map's tones and kits as presets", preset_discovery),
            ("loads a preset onto its part", preset_load),
            ("a preset before the first process() (known issue)", preset_before_first_process),
            ("plays General MIDI when there are no name files", gm_fallback),
            ("deactivates the engine when stopped, never terminates", stop),
        ):
            case(f"{clap}: {name}", fn, v)
    return summary()


if __name__ == "__main__":
    sys.exit(main())

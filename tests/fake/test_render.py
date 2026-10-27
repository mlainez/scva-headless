#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# What the four renderer builds ask of the engine, and what they write, with
# the fake core standing in for SCCore.dll. Runs every case against every
# build that exists: scva-render, scva-render32, and the two Windows builds
# under wine.
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from harness import (RENDER_BLOCK, GM_RESET, GS_RESET, PathArg, case, fake_signal,
                     read_wav, skip, summary)
import harness

MIDI = os.environ.get("SCVA_TEST_MIDI")         # set by run.sh
RATE = 48000
TICK = RATE / 960                               # frames per tick at 120 bpm, 480 ppq


def song(name):
    return PathArg(os.path.join(MIDI, name))


def render(tool, name, *extra, bits="32"):
    out = PathArg(os.path.join(tempfile.mkdtemp(), "out.wav"))
    r = tool.run(["--core", "{core}", "--midi", song(name), "--out", out,
                  "--rate", str(RATE), "--flat-out", "--tail", "0.1",
                  "--bits", bits] + list(extra))
    r.wav = str(out)
    return r


def ran(c, r):
    return c.expect(r.status == 0, f"exit 0, got {r.status}: {r.text.strip()[-300:]}")


# ---- setting the engine up ----------------------------------------------------

def setup_order(c, tool):
    """The rate must be set again after the block size and before activate,
    or the engine renders NaN; activate takes the rate and the block size."""
    r = render(tool, "timing.mid")
    if not ran(c, r):
        return
    a = r.on("A")
    fns = [x.fn for x in a]
    block = max((i for i, f in enumerate(fns) if f == "TG_setMaxBlockSize"), default=None)
    act = fns.index("TG_activate") if "TG_activate" in fns else None
    c.expect(block is not None and act is not None, "a block size and an activate")
    if block is None or act is None:
        return
    c.expect("TG_setSampleRate" in fns[block + 1:act], "the rate set again between block size and activate")
    c.expect(a[act].args == [str(RATE), "4096"], f"activate {RATE} 4096, got {a[act].args}")
    c.expect(r.index(lambda x: x.fn == "TG_Process") > r.index(lambda x: x.fn == "TG_activate"),
             "no rendering before activate")


def gs_reset_first(c, tool):
    r = render(tool, "timing.mid")
    if not ran(c, r):
        return
    resets = [i for i, x in enumerate(r.calls) if x.sysex == GS_RESET]
    note = r.find_midi(0x90, 60)
    c.expect(len(resets) == 1, f"one GS reset, got {len(resets)}")
    c.expect(resets and note and resets[0] < r.calls.index(note[0]),
             "the GS reset before the song's first note")


def gm_reset_option(c, tool):
    r = render(tool, "timing.mid", "--reset", "gm")
    if not ran(c, r):
        return
    c.expect(any(x.sysex == GM_RESET for x in r.calls), "a GM System On")
    c.expect(not any(x.sysex == GS_RESET for x in r.calls), "no GS reset")


def no_reset_option(c, tool):
    r = render(tool, "timing.mid", "--reset", "none")
    if not ran(c, r):
        return
    c.expect(not any(x.sysex in (GS_RESET, GM_RESET) for x in r.calls), "no reset at all")


def every_part_on_the_map(c, tool):
    for name, value in (("default", 0), ("55", 1), ("88", 2), ("88pro", 3), ("8820", 4)):
        r = render(tool, "timing.mid", "--map", name)
        if not ran(c, r):
            return
        first_note = r.calls.index(r.find_midi(0x90, 60)[0])
        for ch in range(16):
            sent = [i for i, x in enumerate(r.calls[:first_note])
                    if x.midi == (0xb0 | ch, 32, value)]
            c.expect(sent, f"--map {name}: channel {ch + 1} put on map {value} before the song starts")


def map_reasserted_before_programs(c, tool):
    """With a map chosen, every program change is preceded by that map's CC32
    on the same channel, so the song's own bank selects cannot move it."""
    r = render(tool, "programs.mid", "--map", "88pro")
    if not ran(c, r):
        return
    for pc in [x for x in r.calls if x.midi and (x.midi[0] & 0xf0) == 0xc0]:
        ch = pc.midi[0] & 0x0f
        prev = r.calls[r.calls.index(pc) - 1]
        c.expect(prev.midi == (0xb0 | ch, 32, 3),
                 f"CC32=3 on channel {ch + 1} right before {pc}, got {prev}")


def default_map_leaves_the_song_alone(c, tool):
    """On the default map the song's own CC32 reaches the engine and nothing
    is inserted before its program changes."""
    r = render(tool, "programs.mid")
    if not ran(c, r):
        return
    c.expect(r.find_midi(0xb1, 32, 2), "the song's own CC32=2 on channel 2")
    start = r.index(lambda x: x.midi == (0xc0, 5, 0))
    inserted = [x for x in r.calls[start - 1:start]
                if x.midi and x.midi[0] == 0xb0 and x.midi[1] == 32]
    c.expect(not inserted, "no CC32 inserted before the first program change")


# ---- the song ----------------------------------------------------------------------

def events_on_time(c, tool):
    """Each event reaches the engine before the block it falls in is rendered,
    and no more than one block early."""
    r = render(tool, "timing.mid")
    if not ran(c, r):
        return
    expected = [(r.find_midi(0x90, 60), 0), (r.find_midi(0xb0, 7, 90), 480),
                (r.find_midi(0x80, 60), 1440)]
    syx = [x for x in r.calls if x.sysex == "f0 41 10 42 12 40 01 30 04 0b f7"]
    expected.append((syx, 960))
    for calls, tick in expected:
        if not c.expect(len(calls) == 1, f"the event at tick {tick} sent once, got {len(calls)}"):
            continue
        when = tick * TICK
        rendered = calls[0].frame
        c.expect(when - RENDER_BLOCK < rendered <= when,
                 f"event at frame {when:.0f} sent after {rendered} frames had been rendered")


def song_sysex_whole(c, tool):
    r = render(tool, "timing.mid")
    if ran(c, r):
        c.expect(any(x.sysex == "f0 41 10 42 12 40 01 30 04 0b f7" for x in r.calls),
                 "the song's SysEx, F0 to F7")


def tail_and_length(c, tool):
    """Rendering runs on for --tail after the last event, and the WAV holds
    exactly what was rendered."""
    r = render(tool, "timing.mid")
    if not ran(c, r):
        return
    last = 1440 * TICK + 0.1 * RATE
    rendered = r.rendered("A")
    c.expect(last <= rendered < last + RENDER_BLOCK,
             f"{last:.0f} to {last + RENDER_BLOCK:.0f} frames rendered, got {rendered}")
    rate, bits, frames = read_wav(r.wav)
    c.expect(rate == RATE and bits == 32, f"a {RATE} Hz float WAV, got {rate} Hz {bits}-bit")
    c.expect(len(frames) == rendered, f"{rendered} frames in the WAV, got {len(frames)}")


def wav_float(c, tool):
    r = render(tool, "timing.mid")
    if not ran(c, r):
        return
    _, _, frames = read_wav(r.wav)
    bad = [n for n, (l, rr) in enumerate(frames) if l != fake_signal(n) or rr != -fake_signal(n)]
    c.expect(not bad, f"the engine's samples unchanged, first difference at frame {bad[:1]}")


def wav_16bit(c, tool):
    r = render(tool, "timing.mid", bits="16")
    if not ran(c, r):
        return
    _, bits, frames = read_wav(r.wav)
    c.expect(bits == 16, f"16-bit, got {bits}")
    bad = [n for n, (l, rr) in enumerate(frames)
           if l != int(fake_signal(n) * 32768) / 32768.0 or rr != -int(fake_signal(n) * 32768) / 32768.0]
    c.expect(not bad, f"samples scaled by 32768, first difference at frame {bad[:1]}")


def two_ports(c, tool):
    """A second port gets a second engine: each engine hears only its own
    port's notes, both hear SysEx, and the output is their sum."""
    r = render(tool, "twoport.mid")
    if not ran(c, r):
        return
    c.expect(r.engines() == ["A", "B"], f"two engines, got {r.engines()}")
    a_note, b_note = r.find_midi(0x90, 60), r.find_midi(0x90, 67)
    c.expect([x.engine for x in a_note] == ["A"], f"port A's note on engine A only, got {a_note}")
    c.expect([x.engine for x in b_note] == ["B"], f"port B's note on engine B only, got {b_note}")
    c.expect([x.engine for x in r.find_midi(0xc0, 33)] == ["B"], "port B's program change on engine B only")
    syx = sorted(x.engine for x in r.calls if x.sysex == "f0 41 10 42 12 40 01 30 04 0b f7")
    c.expect(syx == ["A", "B"], f"the SysEx on both engines, got {syx}")
    resets = sorted(x.engine for x in r.calls if x.sysex == GS_RESET)
    c.expect(resets == ["A", "B"], f"both engines reset, got {resets}")
    _, _, frames = read_wav(r.wav)
    bad = [n for n, (l, _) in enumerate(frames) if abs(l - 2 * fake_signal(n)) > 1e-6]
    c.expect(not bad, f"the two engines' output summed, first difference at frame {bad[:1]}")


def shutdown(c, tool):
    """Every engine is deactivated; the process ends through the engine's
    terminate, once, after that."""
    r = render(tool, "twoport.mid")
    if not ran(c, r):
        return
    for e in ("A", "B"):
        c.expect([x.fn for x in r.on(e)].count("TG_deactivate") == 1, f"engine {e} deactivated once")
    terms = [i for i, x in enumerate(r.calls) if x.fn == "TG_terminate"]
    c.expect(len(terms) == 1 and terms[0] == len(r.calls) - 1, "terminate once, as the last call")
    c.expect("frames" in r.stdout, "the summary line printed before terminate ends the process")


def block_floor(c, tool):
    r = render(tool, "timing.mid", "--maxblock", "100")
    if not ran(c, r):
        return
    c.expect("100" in r.text and "256" in r.text, "a warning naming 100 and 256")
    act = [x for x in r.calls if x.fn == "TG_activate"]
    c.expect(act and act[0].args[1] == "256", f"activate with 256, got {act[0].args if act else None}")


def other_rate(c, tool):
    out = PathArg(os.path.join(tempfile.mkdtemp(), "out.wav"))
    r = tool.run(["--core", "{core}", "--midi", song("timing.mid"), "--out", out,
                  "--rate", "44100", "--flat-out", "--tail", "0"])
    if not ran(c, r):
        return
    act = [x for x in r.calls if x.fn == "TG_activate"]
    c.expect(act and act[0].args[0] == "44100", "activate at 44100")
    off = r.find_midi(0x80, 60)
    c.expect(off and 1.5 * 44100 - RENDER_BLOCK < off[0].frame <= 1.5 * 44100,
             f"the note-off at 1.5 s, frame {1.5 * 44100:.0f}")


def missing_export(c, tool):
    r = tool.run(["--core", "{core}", "--midi", song("timing.mid"), "--out",
                  PathArg(os.path.join(tempfile.mkdtemp(), "o.wav")), "--flat-out"],
                 core=tool.core("-noexport"))
    c.expect(r.status == 1, f"exit 1, got {r.status}")
    c.expect("TG_XPgetCurTotalRunningVoices" in r.text, "the missing export named")


def wrong_width(c, tool, other):
    """A core of the other width is refused, and the message says which
    width this build hosts."""
    r = tool.run(["--core", "{core}", "--midi", song("timing.mid"), "--out",
                  PathArg(os.path.join(tempfile.mkdtemp(), "o.wav")), "--flat-out"],
                 core=other)
    c.expect(r.status == 1, f"exit 1, got {r.status}")
    want = "32-bit" if "32" in tool.name else "64-bit"
    c.expect(want in r.text, f"a message saying it hosts {want} cores: {r.text.strip()[-200:]}")


# ---- native-only options ---------------------------------------------------------------

def init_and_cfg(c, tool):
    r = render(tool, "timing.mid", "--init", "0x10", "--cfg", "0", "1")
    if not ran(c, r):
        return
    init = [x for x in r.calls if x.fn == "TG_initialize"]
    cfg = [x for x in r.calls if x.fn == "TG_XPsetSystemConfig"]
    c.expect(init and init[0].args == ["16"], f"initialize(16), got {init[0].args if init else None}")
    c.expect(cfg and cfg[0].args == ["0", "1"], f"system config 0 1, got {cfg[0].args if cfg else None}")


def dump_option(c, tool):
    r = render(tool, "timing.mid", "--dump", "3")
    if not ran(c, r):
        return
    lines = [l for l in r.stdout.splitlines() if l.strip().startswith("[")]
    c.expect(len(lines) == 3, f"three dumped frames, got {len(lines)}")
    c.expect(lines and "0.00390625" in lines[1], f"frame 1 is the engine's 1/256: {lines[1:2]}")


# ---- Linux and Windows renderers agree ---------------------------------------------------

def midi_and_blocks(r):
    return [(x.engine, x.fn, tuple(a for a in x.args if not a.startswith("when=")), x.frame)
            for x in r.calls if x.fn in ("TG_ShortMidiIn", "TG_LongMidiIn", "TG_Process")]


def same_as_native(c, native, windows):
    for name in ("timing.mid", "programs.mid", "twoport.mid"):
        for extra in ([], ["--map", "88"]):
            a, b = render(native, name, *extra), render(windows, name, *extra)
            if not (ran(c, a) and ran(c, b)):
                return
            c.expect(midi_and_blocks(a) == midi_and_blocks(b),
                     f"{name} {' '.join(extra)}: the same messages and blocks from both builds")


# ---- --play on Windows ---------------------------------------------------------

def play(tool, name, *extra):
    return tool.run(["--core", "{core}", "--midi", song(name), "--play",
                     "--rate", str(RATE), "--tail", "0.1"] + list(extra))


def no_audio_device(tool):
    try:
        return "cannot open an audio device" in play(tool, "timing.mid").text
    except Exception:               # a hang is for the case to report
        return False


def play_same_as_out(c, tool):
    """Played to the sound card, a song drives the engine exactly as when it
    is written to a file, and is played to its end."""
    for name in ("timing.mid", "twoport.mid"):
        p, o = play(tool, name), render(tool, name)
        if not (ran(c, p) and ran(c, o)):
            return
        c.expect(midi_and_blocks(p) == midi_and_blocks(o),
                 f"{name}: the same messages and blocks as --out")
        frames = [l for l in p.text.splitlines() if l.startswith("played: ")]
        written = [l for l in o.text.splitlines() if ": " in l and " frames at " in l]
        c.expect(frames and written and frames[0].split(": ", 1)[1] == written[-1].split(": ", 1)[1],
                 f"{name}: the same summary as --out, got {frames} and {written[-1:]}")


def main():
    tools = {t.name: t for t in harness.RENDERERS}
    for t in harness.RENDERERS:
        if not t.built():
            skip(t.name, "not built")
            continue
        if not os.path.exists(t.core()):
            skip(t.name, "fake core not built (make fakecores)")
            continue
        for name, fn in (
            ("sets the engine up in the order it needs", setup_order),
            ("resets the engine to GS before the song", gs_reset_first),
            ("--reset gm sends GM System On instead", gm_reset_option),
            ("--reset none sends no reset", no_reset_option),
            ("puts every part on the chosen map before the song", every_part_on_the_map),
            ("re-asserts the chosen map before every program change", map_reasserted_before_programs),
            ("on the default map, passes the song's bank selects through", default_map_leaves_the_song_alone),
            ("sends each event on time, at most one block early", events_on_time),
            ("sends the song's SysEx whole", song_sysex_whole),
            ("renders the tail and writes exactly what it rendered", tail_and_length),
            ("writes the engine's float samples unchanged", wav_float),
            ("writes 16-bit samples scaled by 32768", wav_16bit),
            ("plays a two-port song on two engines and sums them", two_ports),
            ("deactivates every engine and terminates once, last", shutdown),
            ("raises a block below 256 to 256", block_floor),
            ("renders at the rate asked for", other_rate),
            ("names an export the core lacks", missing_export),
        ):
            case(f"{t.name}: {name}", fn, t)
        other = os.path.join(harness.FAKE, {"linux64": "linux32", "linux32": "linux64",
                                            "win64": "win32", "win32": "win64"}[t.fake], "SCCore.dll")
        if not t.windows or "32" in t.name:
            case(f"{t.name}: refuses a core of the other width", wrong_width, t, other)
        if not t.windows:
            case(f"{t.name}: --init and --cfg reach the engine", init_and_cfg, t)
            case(f"{t.name}: --dump prints the first frames", dump_option, t)
        if t.windows:
            if no_audio_device(t):
                skip(f"{t.name}: --play drives the engine as --out does", "no audio device under wine")
            else:
                case(f"{t.name}: --play drives the engine as --out does", play_same_as_out, t)
    for native, windows in (("scva-render", "scva-render.exe"), ("scva-render32", "scva-render32.exe")):
        if tools[native].built() and tools[windows].built():
            case(f"{windows} drives the engine exactly as {native}", same_as_native,
                 tools[native], tools[windows])
    return summary()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# The VSC engine as a plugin, in the 32-bit LV2 bundle and CLAPs, driven by
# the minimal test hosts (tests/plugins): which files offer it, that it plays
# at the host's rate whether VSC has it or not, that the Tone Set parameter
# changes the set, and that the presets are the engine's own names. Needs
# SCVA_TEST_VSC_DIR; the Windows CLAP also wine.
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
BUILD = os.path.join(ROOT, "build")
sys.path.insert(0, os.path.join(ROOT, "tests", "fake"))
from harness import case, skip, summary  # noqa: E402

# Program 0 in bank 24 is "Piano + Str." on SC-88Pro; SC-55 has no such
# variation and plays Piano 1. Common tones sound the same in every set.
SET_SPECIFIC = ["midi 0 b0 00 18", "midi 1 c0 00", "midi 10 90 3c 64"]


class Host:
    """One test host run: its script in, its output parsed."""

    def __init__(self, kind, plugin, index, rate, script):
        if kind == "lv2":
            cmd = [os.path.join(BUILD, "test", "lv2_host32"), os.path.join(BUILD, plugin),
                   str(index), str(rate)]
        elif kind == "clap":
            cmd = [os.path.join(BUILD, "test", "clap_host32"), os.path.join(BUILD, plugin),
                   str(index), str(rate)]
        else:
            cmd = ["wine", os.path.join(BUILD, "test", "clap_host_win32.exe"),
                   os.path.join(BUILD, plugin), str(index), str(rate)]
        script = "\n".join(script) + "\nquit\n"
        if kind != "lv2":
            script = script.replace("run ", "process ").replace("port map ", "set 0 ")
        env = dict(os.environ, SCVA_DLL_DIR=DATA, WINEDEBUG="-all",
                   WINEPREFIX=os.environ.get("SCVA_TEST_WINEPREFIX",
                                             os.path.join(BUILD, "test", "wineprefix")))
        p = subprocess.run(cmd, input=script.encode(), capture_output=True, env=env, timeout=120)
        self.out = p.stdout.decode(errors="replace").replace("\r", "")
        self.lines = self.out.splitlines()
        self.peaks = [float(l.split()[1]) for l in self.lines if l.startswith("peak ")]
        self.samples = "\n".join(l for l in self.lines if l.startswith("L"))

    def digest(self):
        return hashlib.md5(self.samples.encode()).hexdigest()


def plays_at(kind, plugin, rate):
    def check(c):
        h = Host(kind, plugin, 1, rate, ["run 4096", "peak", "midi 0 90 3c 64", "run 4096", "peak",
                                         "run 4096", "peak"])
        c.expect(len(h.peaks) == 3, f"three blocks: {h.out[-300:]}")
        if len(h.peaks) == 3:
            c.expect(h.peaks[0] == 0.0, f"silent before the note, got {h.peaks[0]}")
            c.expect(h.peaks[1] > 0.05 and h.peaks[2] > 0.05, f"the note heard: {h.peaks}")
    return check


def the_set_changes_the_sound(kind, plugin):
    def check(c):
        h55 = Host(kind, plugin, 1, 44100, ["port map 1"] + SET_SPECIFIC + ["run 4096", "run 4096"])
        h88p = Host(kind, plugin, 1, 44100, ["port map 3"] + SET_SPECIFIC + ["run 4096", "run 4096"])
        c.expect(h55.samples and h88p.samples, "both rendered")
        c.expect(h55.digest() != h88p.digest(), "SC-55 and SC-88Pro sound different on a tone only one has")
    return check


def a_set_change_mid_session_is_the_new_set(kind, plugin):
    def check(c):
        fresh = Host(kind, plugin, 1, 44100, ["port map 3", "run 4096"] + SET_SPECIFIC + ["run 4096", "run 4096"])
        switched = Host(kind, plugin, 1, 44100, ["port map 1", "run 4096", "port map 3", "run 4096"]
                        + SET_SPECIFIC + ["run 4096", "run 4096"])
        tail = lambda h: "\n".join(h.samples.splitlines()[-2:])
        c.expect(tail(fresh) and tail(fresh) == tail(switched),
                 "after switching to SC-88Pro it plays as a fresh SC-88Pro instance")
    return check


def offers(kind, plugin, index, uri_or_id, expect):
    def check(c):
        if kind == "lv2":
            h = Host(kind, plugin, index, 48000, [])
            got = any(l == f"uri {uri_or_id}" for l in h.lines)
        else:
            h = Host(kind, plugin, 0, 48000, ["plugins"])
            got = any(l.startswith(f"plugin {uri_or_id}|") for l in h.lines)
        c.expect(got == expect, f"{'offers' if expect else 'does not offer'} {uri_or_id}: {h.out[-300:]}")
    return check


def presets_are_the_engines_names(kind, plugin):
    def check(c):
        h = Host(kind, plugin, 1, 48000, ["params", "presets"])
        presets = [l for l in h.lines if l.startswith("preset ")]
        c.expect(any(l.startswith("preset SC-55 001 Piano 1|") for l in presets),
                 f"SC-55's Piano 1: {presets[:3]}")
        c.expect(any(l.startswith("preset SC-88Pro 001-024 Piano + Str.|") for l in presets),
                 "SC-88Pro's Piano + Str.")
        c.expect(not any("SC-8820" in l for l in presets), "nothing of SC-8820's")
        c.expect(any(l.startswith("param 0 Tone Map|0|3|") for l in h.lines),
                 "a Tone Map from Default to SC-88Pro")
    return check


def a_preset_plays_its_tone(kind, plugin):
    def check(c):
        loaded = Host(kind, plugin, 1, 44100, ["load 3.24.0.t", "run 4096", "midi 10 90 3c 64",
                                               "run 4096", "run 4096"])
        piano = Host(kind, plugin, 1, 44100, ["load 3.0.0.t", "run 4096", "midi 10 90 3c 64",
                                              "run 4096", "run 4096"])
        c.expect(any(l.endswith(" ok") for l in loaded.lines if l.startswith("load ")),
                 f"the preset loaded: {loaded.out[:200]}")
        c.expect(loaded.digest() != piano.digest(), "Piano + Str. is not Piano 1")
    return check


if __name__ == "__main__":
    DATA = os.environ.get("SCVA_TEST_VSC_DIR")
    if not DATA:
        print("skip test_plugins_vsc (set SCVA_TEST_VSC_DIR)")
        sys.exit(0)
    VSC_LV2 = "https://github.com/mlainez/scva-headless#vsc"
    VSC_CLAP = "com.github.mlainez.scva-headless.vsc"
    have = lambda f: os.path.exists(os.path.join(BUILD, f))
    tests = []
    if have("scva32.lv2/scva.so") and have("test/lv2_host32"):
        tests += [
            ("scva32.lv2 offers the VSC plugin", offers("lv2", "scva32.lv2", 1, VSC_LV2, True)),
            ("scva32.lv2 VSC plays at 48000 Hz, resampled", plays_at("lv2", "scva32.lv2", 48000)),
            ("scva32.lv2 VSC plays at 44100 Hz, its own", plays_at("lv2", "scva32.lv2", 44100)),
            ("scva32.lv2 VSC: the tone set changes the sound", the_set_changes_the_sound("lv2", "scva32.lv2")),
            ("scva32.lv2 VSC: a set changed mid-session is that set",
             a_set_change_mid_session_is_the_new_set("lv2", "scva32.lv2")),
        ]
    if have("scva.lv2/scva.so") and have("test/lv2_host64"):
        tests.append(("scva.lv2, 64-bit, does not offer it", lambda c: c.expect(
            "no descriptor" in subprocess.run(
                [os.path.join(BUILD, "test", "lv2_host64"), os.path.join(BUILD, "scva.lv2"), "1"],
                input=b"quit\n", capture_output=True).stdout.decode(), "no second descriptor")))
    if have("scva32.clap") and have("test/clap_host32"):
        tests += [
            ("scva32.clap offers the VSC plugin", offers("clap", "scva32.clap", 1, VSC_CLAP, True)),
            ("scva32.clap VSC plays at 48000 Hz, resampled", plays_at("clap", "scva32.clap", 48000)),
            ("scva32.clap VSC: the tone set changes the sound", the_set_changes_the_sound("clap", "scva32.clap")),
            ("scva32.clap VSC: its presets are the engine's names",
             presets_are_the_engines_names("clap", "scva32.clap")),
            ("scva32.clap VSC: a preset plays its tone", a_preset_plays_its_tone("clap", "scva32.clap")),
        ]
    if have("scva.clap") and have("test/clap_host64"):
        tests.append(("scva.clap, 64-bit, does not offer it", offers("clap", "scva.clap", 0, VSC_CLAP, False)))
    if have("scva-win32.clap") and have("test/clap_host_win32.exe") and shutil.which("wine"):
        tests += [
            ("scva-win32.clap offers the VSC plugin", offers("wine", "scva-win32.clap", 1, VSC_CLAP, True)),
            ("scva-win32.clap VSC plays at 48000 Hz, resampled", plays_at("wine", "scva-win32.clap", 48000)),
        ]
    if not tests:
        skip("the VSC plugins", "no 32-bit plugin and test host built")
    for name, fn in tests:
        case(name, fn)
    sys.exit(summary())

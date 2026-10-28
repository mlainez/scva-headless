#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# scva-daemon32 with --engine vsc: it comes up at VSC's own rate, and a map
# typed at its console or sent to its socket changes VSC's one tone set for
# every part. Needs SCVA_TEST_VSC_DIR (vsc.sys and the tone sets) and a
# sequencer (/dev/snd/seq). The audio itself is test-vsc's renders.
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
DAEMON = os.path.join(ROOT, "build", "scva-daemon32")
sys.path.insert(0, os.path.join(ROOT, "tests", "fake"))
from harness import case, skip, summary  # noqa: E402


class Daemon:
    def __init__(self, *args):
        self.dir = tempfile.mkdtemp()
        self.sock = os.path.join(self.dir, "ctl.sock")
        self.lines = []
        self.proc = subprocess.Popen(
            [DAEMON, "--engine", "vsc", "--core", os.path.join(DATA, "vsc.sys"),
             "--pcm", "null", "--name", f"vsctest{os.getpid()}", "--control", self.sock]
            + list(args),
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        threading.Thread(target=self._read, daemon=True).start()
        deadline = time.time() + 15
        while time.time() < deadline and self.proc.poll() is None:
            if any("control socket" in l for l in self.lines):
                break
            time.sleep(0.05)

    def _read(self):
        for raw in self.proc.stderr:
            self.lines.append(raw.decode(errors="replace").rstrip("\n"))

    @property
    def up(self):
        return self.proc.poll() is None and any("control socket" in l for l in self.lines)

    def console(self, text):
        n = len(self.lines)
        self.proc.stdin.write((text + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.time() + 10
        while time.time() < deadline and len(self.lines) == n:
            time.sleep(0.05)
        return "\n".join(self.lines[n:])

    def send(self, text):
        p = subprocess.run([DAEMON, "--send", text, "--control", self.sock],
                           capture_output=True, timeout=15)
        return p.returncode, p.stdout.decode(errors="replace")

    def stop(self):
        if self.proc.poll() is None:
            self.send("q")
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        return self.proc.returncode

    def text(self):
        return "\n".join(self.lines)


def up(c, d):
    return c.expect(d.up, f"the daemon up: {d.text()[-400:]}")


def comes_up_at_vsc_rate(c):
    d = Daemon()
    try:
        if up(c, d):
            c.expect("22050 Hz" in d.text(), f"VSC's default rate: {d.text()[-300:]}")
    finally:
        c.expect(d.stop() == 0, "exit 0 after q")


def another_vsc_rate(c):
    d = Daemon("--rate", "44100")
    try:
        if up(c, d):
            c.expect("44100 Hz" in d.text(), f"44100 Hz: {d.text()[-300:]}")
    finally:
        d.stop()


def a_rate_vsc_lacks(c):
    p = subprocess.run([DAEMON, "--engine", "vsc", "--core", os.path.join(DATA, "vsc.sys"),
                        "--pcm", "null", "--rate", "48000"], capture_output=True, timeout=15)
    err = p.stderr.decode(errors="replace")
    c.expect(p.returncode == 2, f"exit 2, got {p.returncode}")
    c.expect("22050" in err and "44100" in err, f"the rates it has: {err!r}")


def a_typed_map_changes_the_set(c):
    d = Daemon()
    try:
        if not up(c, d):
            return
        c.expect("map 2" in d.console("88"), "a reply naming map 2")
        c.expect("map: " + " ".join(["2"] * 16) in d.console(""), "every part on map 2")
        c.expect("map 3" in d.console("88pro"), "and back to 88pro")
    finally:
        d.stop()


def a_sent_map_changes_the_set(c):
    d = Daemon()
    try:
        if not up(c, d):
            return
        status, out = d.send("55")
        c.expect(status == 0 and "map 1" in out, f"the reply, got {status} {out!r}")
        status, out = d.send("")
        c.expect("map: " + " ".join(["1"] * 16) in out, f"every part on map 1: {out!r}")
    finally:
        d.stop()


def one_part_alone_is_refused(c):
    d = Daemon()
    try:
        if not up(c, d):
            return
        c.expect("every part" in d.console("10 55"), "a refusal: one set for every part")
        c.expect("map: " + " ".join(["0"] * 16) in d.console(""), "nothing changed")
    finally:
        d.stop()


def a_map_vsc_lacks_is_refused(c):
    d = Daemon()
    try:
        if not up(c, d):
            return
        reply = d.console("8820")
        c.expect("88pro" in reply, f"the sets VSC has: {reply!r}")
        c.expect("map: " + " ".join(["0"] * 16) in d.console(""), "nothing changed")
        c.expect(d.up, "still running")
    finally:
        d.stop()


def map_at_start(c):
    d = Daemon("--map", "88")
    try:
        if up(c, d):
            c.expect("map: " + " ".join(["2"] * 16) in d.console(""), "every part on map 2")
    finally:
        d.stop()


if __name__ == "__main__":
    DATA = os.environ.get("SCVA_TEST_VSC_DIR")
    if not DATA:
        print("skip test_daemon_vsc (set SCVA_TEST_VSC_DIR)")
        sys.exit(0)
    if not os.path.exists(DAEMON):
        skip("scva-daemon32 with --engine vsc", "scva-daemon32 not built (make daemon32)")
        sys.exit(summary())
    if not os.access("/dev/snd/seq", os.R_OK | os.W_OK):
        skip("scva-daemon32 with --engine vsc", "no access to /dev/snd/seq")
        sys.exit(summary())
    for name, fn in (
        ("comes up at VSC's own rate", comes_up_at_vsc_rate),
        ("takes another rate VSC has", another_vsc_rate),
        ("refuses a rate VSC lacks, naming the ones it has", a_rate_vsc_lacks),
        ("changes the tone set on a map typed at its console", a_typed_map_changes_the_set),
        ("changes the tone set on a map sent to its socket", a_sent_map_changes_the_set),
        ("refuses a map for one part alone", one_part_alone_is_refused),
        ("refuses a map VSC has no set for, and keeps playing", a_map_vsc_lacks_is_refused),
        ("starts on the set --map names", map_at_start),
    ):
        case(f"scva-daemon32 --engine vsc: {name}", fn)
    sys.exit(summary())

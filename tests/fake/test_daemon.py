#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# scva-daemon running against the fake core: what it asks of the engine when
# MIDI arrives on its sequencer port, when the console or --send changes a
# map, and how it starts and stops. Audio goes to ALSA's null device.
#
# Needs the ALSA sequencer (/dev/snd/seq) and aplaymidi; where either is
# missing the tests are skipped, CI included, since that is the machine and
# not the build.
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import harness
from harness import GS_RESET, Tool, case, parse_trace, skip, summary

MIDI = os.environ.get("SCVA_TEST_MIDI")
DAEMONS = [Tool("scva-daemon", "linux64", False), Tool("scva-daemon32", "linux32", False)]


class Daemon:
    """One running daemon, its stderr, its trace and its control socket."""

    def __init__(self, tool, *args, name=None):
        self.tool = tool
        self.dir = tempfile.mkdtemp()
        self.name = name or f"scvatest{os.getpid()}{len(args)}{int(time.time() * 1000) % 100000}"
        self.sock = os.path.join(self.dir, "ctl.sock")
        self.trace_path = os.path.join(self.dir, "trace")
        self.lines = []
        fd = os.open(self.trace_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        env = dict(os.environ)
        env.pop("SCVA_DLL_DIR", None)
        self.proc = subprocess.Popen(
            [os.path.abspath(tool.path), "--core", os.path.abspath(tool.core()),
             "--pcm", "null", "--name", self.name, "--control", self.sock] + list(args),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, cwd=self.dir, pass_fds=(3,), preexec_fn=lambda: os.dup2(fd, 3))
        os.close(fd)
        threading.Thread(target=self._read, daemon=True).start()
        self.client = None
        deadline = time.time() + 15
        while time.time() < deadline and self.proc.poll() is None:
            m = [re.search(r"client (\d+)", l) for l in self.lines]
            if any(m) and any("control socket" in l for l in self.lines):
                self.client = int(next(x for x in m if x).group(1))
                break
            time.sleep(0.05)

    def _read(self):
        for raw in self.proc.stderr:
            self.lines.append(raw.decode(errors="replace").rstrip("\n"))

    @property
    def up(self):
        return self.client is not None and self.proc.poll() is None

    def play(self, song):
        subprocess.run(["aplaymidi", "-p", f"{self.client}:0", os.path.join(MIDI, song)],
                       capture_output=True, timeout=30)
        time.sleep(0.3)                     # let the daemon drain its port

    def console(self, text):
        n = len(self.lines)
        self.proc.stdin.write((text + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.time() + 5
        while time.time() < deadline and len(self.lines) == n:
            time.sleep(0.05)
        return "\n".join(self.lines[n:])

    def send(self, text):
        p = subprocess.run([os.path.abspath(self.tool.path), "--send", text, "--control", self.sock],
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

    def calls(self):
        return parse_trace(open(self.trace_path, errors="replace").read())

    def text(self):
        return "\n".join(self.lines)


def midi_calls(calls):
    return [c for c in calls if c.midi]


def just_before(calls, status, d1):
    """The engine call right before each (status, d1) short message."""
    out = []
    for i, c in enumerate(calls):
        if c.midi and c.midi[0] == status and c.midi[1] == d1 and i:
            out.append(calls[i - 1])
    return out


def started(c, d):
    return c.expect(d.up, f"the daemon up with a port and a socket: {d.text()[-400:]}")


# ---- startup and shutdown ----------------------------------------------------

def setup_and_stop(c, tool):
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    status = d.stop()
    calls = d.calls()
    fns = [x.fn for x in calls]
    c.expect(status == 0, f"exit 0 after q, got {status}")
    block = max(i for i, f in enumerate(fns) if f == "TG_setMaxBlockSize")
    act = fns.index("TG_activate")
    c.expect("TG_setSampleRate" in fns[block + 1:act], "the rate set again between block size and activate")
    c.expect(calls[act].args == ["48000", "256"], f"activate 48000 256, got {calls[act].args}")
    c.expect(any(x.sysex == GS_RESET for x in calls), "a GS reset")
    c.expect("TG_deactivate" in fns, "the engine deactivated when stopping")
    c.expect("TG_terminate" not in fns, "never terminate: it would exit() under the daemon")
    c.expect(not os.path.exists(d.sock), "the control socket removed")


def renders_blocks(c, tool):
    for args, size in (([], 256), (["--block", "512"], 512), (["--block", "100"], 256)):
        d = Daemon(tool, *args)
        if not started(c, d):
            d.stop(); return
        time.sleep(0.3)
        d.stop()
        sizes = harness.Result(0, "", "", d.calls()).block_sizes()
        c.expect(sizes == [size], f"{' '.join(args) or 'default'}: blocks of {size}, got {sizes}")
        if args == ["--block", "100"]:
            c.expect("100" in d.text() and "256" in d.text(), "a warning naming 100 and 256")


def sigterm_is_clean(c, tool):
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    d.proc.terminate()
    d.proc.wait(timeout=10)
    fns = [x.fn for x in d.calls()]
    c.expect(d.proc.returncode == 0, f"exit 0 on SIGTERM, got {d.proc.returncode}")
    c.expect("TG_deactivate" in fns and "TG_terminate" not in fns, "deactivated, not terminated")


# ---- live MIDI and maps ------------------------------------------------------------

def wire_programs_get_the_map(c, tool):
    """Every program change from the wire is preceded by the part's map, on
    the default map too, so a song's own bank selects cannot move a part."""
    for mapname, value in (("default", 0), ("88", 2)):
        d = Daemon(tool, "--map", mapname)
        if not started(c, d):
            d.stop(); return
        d.play("programs.mid")
        d.stop()
        calls = d.calls()
        for st, d1 in ((0xc0, 5), (0xc1, 40)):
            before = just_before(calls, st, d1)
            c.expect(before and before[0].midi == (0xb0 | (st & 0x0f), 32, value),
                     f"--map {mapname}: CC32={value} right before program {d1} on channel {(st & 15) + 1}, got {before}")


def wire_cc32_does_not_move_the_map(c, tool):
    """A CC32 arriving on the wire reaches the engine but is not adopted: the
    next program change still gets the daemon's own map."""
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    d.play("programs.mid")
    reply = d.console("")
    d.stop()
    calls = d.calls()
    c.expect(any(x.midi == (0xb1, 32, 2) for x in calls), "the wire CC32=2 forwarded to the engine")
    before = just_before(calls, 0xc1, 40)
    c.expect(before and before[0].midi == (0xb1, 32, 0), f"CC32=0 re-asserted before the program, got {before}")
    c.expect("map: 0 0 0" in reply, f"the console still reports map 0 everywhere: {reply!r}")


def console_map_change(c, tool):
    """A map typed on the console moves every part, and re-sends each part's
    last program so the change is heard at once."""
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    d.play("programs.mid")
    n_before = len(d.calls())
    reply = d.console("88")
    d.stop()
    calls = d.calls()[n_before:]
    c.expect("map 2" in reply, f"a reply naming map 2: {reply!r}")
    last_pc = {0: 5, 1: 40}
    for ch in range(16):
        cc = [i for i, x in enumerate(calls) if x.midi == (0xb0 | ch, 32, 2)]
        pc = [i for i, x in enumerate(calls) if x.midi == (0xc0 | ch, last_pc.get(ch, 0), 0)]
        c.expect(cc and pc and pc[0] == cc[0] + 1,
                 f"channel {ch + 1}: CC32=2 then program {last_pc.get(ch, 0)}")


def send_one_channel(c, tool):
    """--send with "<channel> <map>" moves that part only, and the daemon's
    reply comes back to the sender."""
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    time.sleep(0.2)
    n_before = len(d.calls())
    status, out = d.send("10 55")
    state = d.send("")[1]
    d.stop()
    changed = [x for x in midi_calls(d.calls()[n_before:])
               if (x.midi[0] & 0xf0) in (0xb0, 0xc0)]
    c.expect(status == 0 and "channel 10" in out and "map 1" in out, f"the reply, got {status} {out!r}")
    c.expect([x.midi for x in changed] == [(0xb9, 32, 1), (0xc9, 0, 0)],
             f"CC32=1 and a program change on channel 10 only, got {changed}")
    c.expect(state.split()[1:] == ["0"] * 9 + ["1"] + ["0"] * 6, f"the map line reflecting it: {state!r}")


def unknown_console_command_helps(c, tool):
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    reply = d.console("frobnicate")
    d.stop()
    c.expect("<channel" in reply and "q" in reply, f"the command help: {reply!r}")


def two_named_daemons(c, tool):
    """Two daemons with different --name run side by side, each with its own
    sequencer client and control socket."""
    a = Daemon(tool, name=f"scvaA{os.getpid()}")
    b = Daemon(tool, name=f"scvaB{os.getpid()}")
    ok = started(c, a) and started(c, b)
    if ok:
        c.expect(a.client != b.client, "two sequencer clients")
        clients = open("/proc/asound/seq/clients").read()
        c.expect(a.name in clients and b.name in clients, "both named in the sequencer")
        c.expect(a.send("88")[0] == 0 and b.send("55")[0] == 0, "each answering on its own socket")
        c.expect(a.send("")[1].split()[1] == "2" and b.send("")[1].split()[1] == "1",
                 "each keeping its own map")
    a.stop(); b.stop()


def realtime_priority(c, tool):
    """With realtime rights the audio runs SCHED_FIFO at the minimum + 5 and
    says so; without them it runs normally and claims nothing."""
    d = Daemon(tool)
    if not started(c, d):
        d.stop(); return
    with open(f"/proc/{d.proc.pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    policy, rtprio = int(fields[38]), int(fields[37])
    granted = any("realtime priority" in l for l in d.lines)
    time.sleep(0.2)
    d.stop()
    try:
        allowed = os.sched_get_priority_min(os.SCHED_FIFO) + 5 <= int(
            subprocess.run(["sh", "-c", "ulimit -r"], capture_output=True, text=True).stdout or 0)
    except ValueError:
        allowed = False
    if allowed:
        c.expect(policy == 1 and rtprio == 6 and granted,
                 f"SCHED_FIFO at 6 and a message, got policy {policy} priority {rtprio}")
    else:
        c.expect(policy == 0 and not granted, "normal scheduling and no realtime claim")
        print("     (no realtime rights here: only the refused path was checked; "
              "see docs/MANUAL-CHECKS.md)")


def main():
    if not os.path.exists("/dev/snd/seq") or not shutil.which("aplaymidi"):
        print("skip scva-daemon live tests (needs /dev/snd/seq and aplaymidi)")
        return summary()
    for t in DAEMONS:
        if not t.built():
            skip(t.name, "not built")
            continue
        for name, fn in (
            ("sets the engine up, and stops it without terminating", setup_and_stop),
            ("renders in --block sized blocks, at least 256", renders_blocks),
            ("stops cleanly on SIGTERM", sigterm_is_clean),
            ("puts the part's map before every program change from the wire", wire_programs_get_the_map),
            ("forwards a CC32 from the wire without adopting it as the map", wire_cc32_does_not_move_the_map),
            ("moves every part on a console map and re-sends their programs", console_map_change),
            ("moves one part on '--send <channel> <map>' and replies", send_one_channel),
            ("answers an unknown console command with help", unknown_console_command_helps),
            ("runs side by side with another daemon of another name", two_named_daemons),
            ("runs SCHED_FIFO when allowed, and normally when not", realtime_priority),
        ):
            case(f"{t.name}: {name}", fn, t)
    return summary()


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: CC0-1.0
#
# Runs the tools against the fake core (tests/fakecore) and reads back what
# they asked of it. A trace is the list of engine calls, in order, each tagged
# with the engine it went to: A for the first engine loaded, B for a second.
import os
import re
import struct
import subprocess
import tempfile

BUILD = os.environ.get("BUILD", "build")
FAKE = os.path.join(BUILD, "test", "fakecore")
STRICT = os.environ.get("SCVA_TEST_STRICT") == "1"

GS_RESET = "f0 41 10 42 12 40 00 7f 00 41 f7"
GM_RESET = "f0 7e 7f 09 01 f7"
RENDER_BLOCK = 256              # the renderers release MIDI per block of this


class Call:
    def __init__(self, engine, fn, args, frame):
        self.engine, self.fn, self.args = engine, fn, args
        self.frame = frame      # frames this engine had rendered by then

    def __repr__(self):
        return f"{self.engine}:{self.fn} {' '.join(self.args)} @{self.frame}"

    @property
    def midi(self):
        """A short message as (status, data1, data2), else None."""
        if self.fn != "TG_ShortMidiIn":
            return None
        return tuple(int(x, 16) for x in self.args[:3])

    @property
    def sysex(self):
        if self.fn != "TG_LongMidiIn":
            return None
        return " ".join(a for a in self.args if not a.startswith("when="))


def parse_trace(text):
    names, calls, rendered = {}, [], {}
    for line in text.splitlines():
        m = re.match(r"@([0-9a-f]+) (\S+)(.*)", line)
        if not m:
            continue
        base, fn, rest = m.groups()
        if base not in names:
            names[base] = "ABCDEFGH"[len(names)]
        eng = names[base]
        args = rest.split()
        calls.append(Call(eng, fn, args, rendered.get(eng, 0)))
        if fn == "TG_Process":       # "<frames> <count>": count blocks of that size
            rendered[eng] = rendered.get(eng, 0) + int(args[0]) * int(args[1])
    return calls


class Result:
    def __init__(self, status, stdout, stderr, calls):
        self.status, self.stdout, self.stderr, self.calls = status, stdout, stderr, calls

    @property
    def text(self):
        return self.stdout + self.stderr

    def engines(self):
        return sorted({c.engine for c in self.calls})

    def on(self, engine):
        return [c for c in self.calls if c.engine == engine]

    def rendered(self, engine="A"):
        return sum(int(c.args[0]) * int(c.args[1]) for c in self.on(engine) if c.fn == "TG_Process")

    def block_sizes(self, engine="A"):
        return sorted({int(c.args[0]) for c in self.on(engine) if c.fn == "TG_Process"})

    def index(self, pred, engine=None):
        """Position in the whole trace of the first call matching pred."""
        for i, c in enumerate(self.calls):
            if (engine is None or c.engine == engine) and pred(c):
                return i
        return None

    def find_midi(self, status, d1, d2=None, engine=None):
        """Every call carrying this short message."""
        return [c for c in self.calls if c.midi and (engine is None or c.engine == engine)
                and c.midi[0] == status and c.midi[1] == d1
                and (d2 is None or c.midi[2] == d2)]


def read_wav(path):
    """(rate, bits, [(left, right)...]) for a 16-bit PCM or 32-bit float WAV."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a WAV file"
    tag, _, rate = struct.unpack_from("<HHI", data, 20)
    bits = struct.unpack_from("<H", data, 34)[0]
    size = struct.unpack_from("<I", data, 40)[0]
    body = data[44:44 + size]
    if tag == 3:
        vals = struct.unpack("<%df" % (len(body) // 4), body)
    else:
        vals = [v / 32768.0 for v in struct.unpack("<%dh" % (len(body) // 2), body)]
    return rate, bits, list(zip(vals[0::2], vals[1::2]))


def fake_signal(n):
    """What the fake core renders at its frame n (see fakecore.c)."""
    return (n % 64) / 256.0


class PathArg(str):
    """A file path argument, converted for wine when the tool is a .exe."""


_winpaths = {}


def winpath(p, env):
    p = os.path.abspath(p)
    if p not in _winpaths:
        out = subprocess.run(["winepath", "-w", p], capture_output=True, env=env, text=True)
        _winpaths[p] = out.stdout.strip() or "Z:" + p.replace("/", "\\")
    return _winpaths[p]


def wine_env():
    env = dict(os.environ)
    env.update(WINEDEBUG="-all",
               WINEPREFIX=os.environ.get("SCVA_TEST_WINEPREFIX",
                                         os.path.abspath(os.path.join(BUILD, "test", "wineprefix"))))
    return env


class Tool:
    """A binary and the fake core build it loads."""

    def __init__(self, name, fake, windows):
        self.name, self.fake, self.windows = name, fake, windows
        self.path = os.path.join(BUILD, name)

    def built(self):
        return os.path.exists(self.path)

    def core(self, variant=""):
        return os.path.join(FAKE, self.fake + variant, "SCCore.dll")

    def run(self, args, core=None, timeout=120):
        """Runs with args, '{core}' standing for the fake core's path."""
        core = core or self.core()
        tmp = tempfile.mkdtemp()
        log = os.path.join(tmp, "trace")
        if self.windows:
            env = wine_env()
            env.pop("SCVA_DLL_DIR", None)
            env["SCVA_FAKE_LOG"] = winpath(log, env)
            conv = [winpath(core, env) if a == "{core}" else
                    winpath(a, env) if isinstance(a, PathArg) else str(a) for a in args]
            p = subprocess.run(["wine", os.path.abspath(self.path)] + conv,
                               capture_output=True, env=env, cwd=tmp, timeout=timeout)
        else:
            env = dict(os.environ)
            env.pop("SCVA_DLL_DIR", None)
            conv = [os.path.abspath(core) if a == "{core}" else str(a) for a in args]
            fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            p = subprocess.run([os.path.abspath(self.path)] + conv, capture_output=True,
                               env=env, cwd=tmp, timeout=timeout, pass_fds=(3,),
                               preexec_fn=lambda: os.dup2(fd, 3))
            os.close(fd)
        trace = open(log, errors="replace").read() if os.path.exists(log) else ""
        out = p.stdout.decode(errors="replace").replace("\r\n", "\n")
        err = p.stderr.decode(errors="replace").replace("\r\n", "\n")
        return Result(p.returncode, out, err, parse_trace(trace))


RENDERERS = [
    Tool("scva-render", "linux64", False),
    Tool("scva-render32", "linux32", False),
    Tool("scva-render.exe", "win64", True),
    Tool("scva-render32.exe", "win32", True),
]

# ---- reporting --------------------------------------------------------------

passed = failed = skipped = 0


class Check:
    def __init__(self, name):
        self.name, self.problems = name, []

    def expect(self, cond, what):
        if not cond:
            self.problems.append(what)
        return cond


def case(name, fn, *args):
    global passed, failed
    c = Check(name)
    try:
        fn(c, *args)
    except Exception as e:          # a crash in a test is a failure, not a pass
        c.problems.append(f"raised {type(e).__name__}: {e}")
    if c.problems:
        failed += 1
        print(f"FAIL {name}")
        for p in c.problems[:8]:
            print(f"     {p}")
    else:
        passed += 1
        print(f"ok   {name}")


def skip(name, why):
    global skipped, failed
    if STRICT:
        failed += 1
        print(f"FAIL {name} ({why})")
    else:
        skipped += 1
        print(f"skip {name} ({why})")


def summary():
    print(f"{passed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0

# Driving SOUND Canvas VA's core

What the exported API does. Several of these fail silently if you get them
wrong: the engine reports no error and emits plausible-sounding audio.

## The signatures

Read from the functions' own prologues - which registers each one consumes
before it writes anything.

    int  TG_initialize(int timestampSource);
    void TG_setSampleRate(float rate);
    void TG_setMaxBlockSize(int frames);
    int  TG_activate(float rate, int maxBlockSize);
    void TG_ShortMidiIn(unsigned int msg, int whenInSamples);
    void TG_LongMidiIn(const unsigned char *sysex, int whenInSamples);
    void TG_Process(float *left, float *right, int frames);
    int  TG_XPsetSystemConfig(const struct { int on, b; } *);
    void TG_XPgetCurSystemConfig(struct { int on, b; } *);

Three of them are easy to get wrong:

- **`TG_activate`'s first argument is a float in XMM0**, the sample rate, not
  an int mode flag in ECX. It is stored where `TG_ShortMidiIn` and
  `TG_LongMidiIn` divide their timestamps by it.
- **`TG_ShortMidiIn`'s second argument is a timestamp in samples**, not a
  length or a flag. `TG_initialize`'s argument chooses whether the engine
  believes it (bit 0 clear) or uses its own sample counter instead.
- **`TG_LongMidiIn`'s second argument is a timestamp too.** The message length
  is implicit: the engine reads until F7. Passing the byte count puts a
  nonsense time on the event.

Get them wrong and the MIDI queue is timestamped from whatever the compiler
happened to leave in RDX and XMM0, which makes the audio depend on unrelated
code elsewhere in the program.

A SysEx event in a Standard MIDI File is stored **without its leading F0** -
the byte is implied by the event type - so it has to be rebuilt before being
handed to `TG_LongMidiIn`, or the engine ignores it.

`TG_terminate` calls `exit()`. Nothing after it runs, and buffered output is
lost.

## `TG_activate`'s second argument is the block size

`TG_activate` calls `TG_setMaxBlockSize` itself, and that call frees both ring
buffers and allocates them again at four bytes per sample:

    free(ringL); free(ringR);
    ringL = new(n * 4);
    ringR = new(n * 4);

So a small value discards whatever `setMaxBlockSize` was given earlier.
`TG_activate(rate, 1)` gives one float per ring; the engine writes 48 samples
into each anyway, and the two minimum-size allocations land a few floats
apart, so the rings **overlap**. Both are written in the same loop, left
first, so the left write for index `i+k` lands on the right value already
there: the right channel comes back as the left channel `k` samples early, for
all but the last `k` of every 48-sample block. `k` is the allocator's minimum
spacing - 8 floats under glibc, 12 under the Windows heap.

Driven correctly, one note with CC10 panned hard over measures:

| pan | L/R |
|---|---|
| hard left | +23.1 dB |
| centre | -0.1 dB |
| hard right | -30.0 dB |

and the right channel carries no bit-exact copy of the left at any lag.

## `TG_setSampleRate` must be called twice

Once before `TG_setMaxBlockSize`, and again as the **last call before**
`TG_activate`. Set it once, on either side, and the core writes `+/-Inf` from
the second sample and `NaN` after - initialising perfectly and reporting no
error throughout.

| sequence | result |
|---|---|
| `rate, block, rate` | finite |
| `rate, block, config, rate` | finite |
| `rate, block` | diverges |
| `rate, block, config` | diverges |
| `block, rate` | diverges |
| no rate call | diverges |

The function early-returns when the rate it is handed equals the rate already
stored (`ucomiss` then `je`), so a repeated call is only a no-op once the
value has taken; what matters is that a rate call comes after the block size,
because `setMaxBlockSize` and `XPsetSystemConfig` both invalidate the
rate-derived state.

The control that says the rate genuinely arrives rather than the output being
accidentally finite: with one rate call, sample 0 is bit-identical at 22050,
44100 and 96000 Hz. With two it differs at each.

`build/ring-probe --order rbcr` varies this sequence one call at a time.

## The system config is two switches

`TG_XPsetSystemConfig` coerces both fields to booleans (`*p != 0`). The first
is the engine itself: zero and it emits silence. The second changes the
output without changing its level. Neither selects anything larger.

## Tone maps: not reachable from the core

The product's tone file `SCVSC.tnf` is plain text and describes five pages:

    MODULENAME=SC-8820   BANKCONTROLCC#=0   PAGECOUNT=5
    PAGE=0 Default (1262)  PAGE=1 55Map (418)  PAGE=2 88Map (418)
    PAGE=3 88ProMap (774)  PAGE=4 8820Map (1262)

The maps are genuinely different assignments, not nested subsets: 88Map names
a different tone from 8820Map at 95 of the (program, bank) cells they share,
and 55Map at 74 of them - `Piano 1w` against `Upright P w`, `Old Upright`
against `Honky-tonk 2`. 55Map additionally carries 192 cells at banks 126 and
127, the MT-32 compatibility banks.

**Nothing in the core selects between them.** Measured, each against a
baseline render of a tone the maps disagree about:

| tried | result |
|---|---|
| GS Tone Map Number SysEx, `40 1n 42`, values 0-4 | no change |
| `TG_initialize` argument, 0-16 | no change |
| `TG_XPsetSystemConfig`, all four combinations | on/off and one subtle flag |
| every other export | none takes a map |

Bank Select MSB (CC0) does work and reaches the variations within a map - at
program 48 and 80 every bank from 0 to 32 gives different audio - but that is
the variation axis, not the map axis.

So the core gives the SC-8820 map. The Map selector in the product's own
interface is not in `SCCore.dll`; it is in the wrapper, which is behind the
authorisation check.

Not exhaustively ruled out: the GS address space was probed at the documented
tone-map parameter only, not swept.

## How audio comes out

- The engine runs internally at **32 kHz**. `TG_Process` calls a resampler that
  consumes 32 input samples and emits 48 at a 48 kHz output rate, which is why
  the ring holds 48 and the read index moves in steps of 16.
- `TG_Process` **generates on demand**: if the ring cannot satisfy the frames
  asked for, it synthesises more and loops until it can. There is no producer
  thread, so rendering flat out is correct and pacing to wall-clock buys
  nothing.
- It writes exactly the frames requested to each buffer - no overrun, no holes.

## Running the core without wine

`src/pe_loader.c` maps `SCCore.dll` into a Linux process and calls it directly.

Possible because the core is self-contained: **50 imports across five
libraries** - critical sections, events, time, `malloc`/`free`,
`memcpy`/`memset`, C++ exception plumbing, CRT start-up. No file I/O, no
registry, no threads of its own, no GUI, no COM. Its data is inside the 26.7 MB
image.

What it rests on:

- **The calling convention is the compiler's job.** Everything crossing the
  boundary is `__attribute__((ms_abi))`. GCC emits the argument shuffling, the
  32 bytes of shadow space, and spills XMM6-XMM15, RDI and RSI around calls out
  to glibc, because `ms_abi` makes those callee-saved where System V does not.
  No hand-written thunks are needed; the `movaps` pairs are in the prologue.
- **GS points at a TEB we build.** Windows reads its thread block through `GS`;
  Linux keeps TLS in `FS` and leaves `GS` alone. The stack bounds in that block
  must be the real ones from `pthread_getattr_np` - MSVC's stack probes compare
  against them, and a guess sends a deep call through the floor.
- **TLS at `gs:[0x58]`.** Without it the first thread-local access is
  `mov (%rax,%rcx,8),%rbx` with `rax = 0`.
- **Condition variables.** The image fetches `InitializeConditionVariable`,
  `SleepConditionVariableCS` and `WakeAllConditionVariable` through
  `GetProcAddress` and calls what it is handed, so they must resolve. They are
  pthreads underneath.
- **Zeroed allocations**, belt and braces: filling fresh allocations with
  `0xAA` instead changes neither channel by a single sample.

Two hooks stay in the loader, both off unless asked for. `PE_TRACE_MALLOC` logs
every allocation with the image-relative address of whoever asked for it.
`PE_MALLOC_FILL=0xAA` replaces the zeroing with a pattern, so "does the engine
read memory it never wrote" has an answer rather than an opinion.

A 5,840,640-frame render is identical byte for byte between `scva-native` and
`wine scva_render.exe`, and identical run to run on both.

## The VST route is shut

`Wrapper.dll` exports its VST entry as `R2RPluginMain` rather than
`VSTPluginMain`, so no host opens it unaided. `src/scva_vst_shim.c` publishes
the expected name and forwards, resolving `Wrapper.dll` next to itself rather
than through the registry. The plug-in then opens, identifies itself, and
crashes inside `effOpen` on an **authorisation check** (`auth_strings_*.bin`,
`Authorize.nib`). The third-party shim that bypasses that check fails its own
`DllMain` under wine, and building on the bypass is not something this project
will do.

## A detail worth keeping

The Mac build ships **32 cores**, `SCCore00` through `SCCore31`, all exactly
1265.1 KB and all byte-different: the same code with a different embedded
constant, so each loads as a separate library with its own globals. The engine
is single-instance by design and Roland multi-instances it by shipping copies.
The Windows `SCCore.dll` is one 26.7 MB file carrying code and data together.

## Standing

Roland's binaries were read statically to answer questions measurement alone
could not: which registers each exported function consumes, which globals
`TG_Process` loads before it copies, and what `TG_activate` does with its
arguments. These are calling-convention and allocation facts about a published
C API, not algorithms, and none is reproduced beyond the few lines quoted here.

# Driving SOUND Canvas VA's core

These fail silently: the engine reports no error and emits plausible audio.

## Signatures

Read from the functions' prologues - which registers each consumes before it
writes anything.

    int  TG_initialize(int timestampSource);
    void TG_setSampleRate(float rate);
    void TG_setMaxBlockSize(int frames);
    int  TG_activate(float rate, int maxBlockSize);
    void TG_ShortMidiIn(unsigned int msg, int whenInSamples);
    void TG_LongMidiIn(const unsigned char *sysex, int whenInSamples);
    void TG_Process(float *left, float *right, int frames);
    int  TG_XPsetSystemConfig(const struct { int on, b; } *);
    void TG_XPgetCurSystemConfig(struct { int on, b; } *);

The easy ones to get wrong:

- `TG_activate`'s first argument is a **float in XMM0**, the sample rate, not
  an int flag in ECX. It is stored where the MIDI functions divide their
  timestamps by it.
- `TG_ShortMidiIn`'s second argument is a **timestamp in samples**.
  `TG_initialize`'s argument chooses whether the engine believes it (bit 0
  clear) or uses its own counter.
- `TG_LongMidiIn`'s second argument is a **timestamp too**. The length is
  implicit: the engine reads until F7.

Get these wrong and the MIDI queue is timestamped from whatever the compiler
left in RDX and XMM0, which makes the audio depend on unrelated code.

An SMF stores a SysEx **without its leading F0**, so it must be rebuilt before
`TG_LongMidiIn` or the engine ignores it.

`TG_terminate` calls `exit()`. Nothing after it runs.

## TG_activate's second argument is the block size

`TG_activate` calls `TG_setMaxBlockSize` itself, and that frees both ring
buffers and allocates them again at four bytes per sample:

    free(ringL); free(ringR);
    ringL = new(n * 4);
    ringR = new(n * 4);

`TG_activate(rate, 1)` gives one float per ring. The engine writes 48 samples
into each anyway, and two minimum-size allocations land a few floats apart, so
the rings **overlap**: the right channel comes back as the left channel `k`
samples early, for all but the last `k` of every 48-sample block. `k` is the
allocator's minimum spacing - 8 floats under glibc, 12 under the Windows heap.

Driven correctly, one note with CC10 panned hard over:

| pan | L/R |
|---|---|
| hard left | +23.1 dB |
| centre | -0.1 dB |
| hard right | -30.0 dB |

## TG_setSampleRate must be called twice

Once before `TG_setMaxBlockSize`, and again as the last call before
`TG_activate`. Otherwise the core writes `+/-Inf` from the second sample and
`NaN` after, reporting no error.

| sequence | result |
|---|---|
| `rate, block, rate` | finite |
| `rate, block, config, rate` | finite |
| `rate, block` | diverges |
| `rate, block, config` | diverges |
| `block, rate` | diverges |
| no rate call | diverges |

It early-returns when the rate equals the one already stored (`ucomiss`, `je`),
so what matters is that a rate call comes *after* the block size:
`setMaxBlockSize` and `XPsetSystemConfig` both invalidate the rate-derived
state.

Control: with one rate call, sample 0 is bit-identical at 22050, 44100 and
96000 Hz; with two it differs at each.

## Tone maps

Selected by **Bank Select LSB (CC32)**, per part, latched by a program change:

| CC32 | map |
|---|---|
| 0 | Default, identical to 8820Map |
| 1 | 55Map |
| 2 | 88Map |
| 3 | 88ProMap |
| 4 | 8820Map |
| >4 | clamps to 4 |

These are the page numbers in the product's own `SCVSC.tnf`, which is plain
text: `MODULENAME=SC-8820`, `BANKCONTROLCC#=0`, five pages of 1262, 418, 418,
774 and 1262 tones.

The maps are not subsets: 88Map names a different tone from 8820Map at 95 of
the cells they share. Validated against the tone file on 17 cells - every cell
the file says the two maps share renders byte-identical, every cell it says
they disagree on renders differently, in both directions.

A GS reset returns every part to the default map, so the map has to be sent
again before each program change rather than once at the start.

Not the selector, measured against a tone the maps disagree about: the GS tone
map parameter `40 1n 42` (no effect), `TG_initialize` 0-16 (no effect),
`TG_XPsetSystemConfig` (field 1 is an enable, field 0 silences the engine),
Bank Select MSB (the variation axis within a map).

## How audio comes out

- Internally **32 kHz**. The resampler consumes 32 input samples and emits 48
  at 48 kHz output, which is why the ring holds 48 and the read index moves in
  steps of 16.
- `TG_Process` **generates on demand**: if the ring cannot satisfy the frames
  asked for it synthesises more and loops. No producer thread, so rendering
  flat out is correct.
- It writes exactly the frames requested to each buffer - no overrun, no holes.

## Running the core without wine

`src/pe_loader.c` maps `SCCore.dll` into a Linux process and calls it directly.
The core imports **50 functions** and does no file I/O, no registry, no threads
of its own, no GUI, no COM; its data is inside the 26.7 MB image.

- Everything crossing the boundary is `__attribute__((ms_abi))`. GCC emits the
  argument shuffling, the shadow space, and spills XMM6-XMM15, RDI and RSI
  around calls out to glibc. No thunks needed.
- `GS` points at a TEB we build. The stack bounds in it must be the real ones
  from `pthread_getattr_np`: MSVC's stack probes compare against them.
- TLS pointer array at `gs:[0x58]`.
- The image fetches the condition-variable trio through `GetProcAddress` and
  calls what it is handed, so they must resolve.
- `malloc` zeroes, because Windows hands back fresh pages. Filling with `0xAA`
  instead changes neither channel by a sample, so nothing reads uninitialised
  memory.

A 5,840,640-frame render is identical byte for byte between `scva-native` and
`wine scva_render.exe`, and identical run to run on both.

## The VST route is shut

`Wrapper.dll` exports its VST entry as `R2RPluginMain`, so no host opens it
unaided. `src/scva_vst_shim.c` publishes the expected name and forwards; the
plug-in then opens and crashes inside `effOpen` on an authorisation check
(`auth_strings_*.bin`, `Authorize.nib`). The core API gives the same audio.

## Standing

Roland's binaries were read statically to answer what measurement could not:
which registers each exported function consumes, which globals `TG_Process`
loads before it copies, and what `TG_activate` does with its arguments. These
are calling-convention and allocation facts about a published C API, not
algorithms.

# Driving SOUND Canvas VA's core

What the exported API does. Two of these are load-bearing and fail silently if
you get them wrong.

## `TG_activate`'s second argument is the block size

`TG_activate(mode, n)` calls `TG_setMaxBlockSize(n)` itself, and that call
frees both ring buffers and allocates them again at four bytes per sample:

    free(ringL); free(ringR);
    ringL = new(n * 4);
    ringR = new(n * 4);

Call it as `TG_activate(0, maxblock)`. The second argument reads like a mode
flag and is not one: a small value discards whatever `setMaxBlockSize` was
given earlier and replaces the rings with buffers that size.

`TG_activate(0, 1)` gives one float per ring. The engine writes 48 samples into
each anyway, and the two minimum-size allocations land a few floats apart, so
the rings **overlap**. Both are written in the same loop, left first, so the
left write for index `i+k` lands on the right value already there: the right
channel comes back as the left channel `k` samples early, for all but the last
`k` of every 48-sample block. `k` is the allocator's minimum spacing - 8 floats
under glibc, 12 under the Windows heap.

Driven correctly, one note with CC10 panned hard over measures:

| pan | L/R |
|---|---|
| hard left | +23.0 dB |
| centre | -0.1 dB |
| hard right | -29.9 dB |

and the right channel carries no bit-exact copy of the left at any lag.

## `TG_setSampleRate` must be called twice

Once before `TG_setMaxBlockSize`, and again as the **last call before**
`TG_activate`. Set it once, on either side, and the core writes `+/-Inf` from
the second sample and `NaN` after - initialising perfectly and reporting no
error throughout.

Measured one variable at a time:

| sequence | result |
|---|---|
| `rate, block, rate` | finite |
| `rate, block, config, rate` | finite |
| `rate, block` | diverges |
| `rate, block, config` | diverges |
| `block, rate` | diverges |
| no rate call | diverges |

`setMaxBlockSize` and `XPsetSystemConfig` both invalidate the rate-derived
state, and a rate call is still needed before `setMaxBlockSize`, which is why
`block, rate` alone fails.

The control that says the rate genuinely arrives rather than the output being
accidentally finite: with one rate call, sample 0 is bit-identical at 22050,
44100 and 96000 Hz. With two it differs at each.

`build/ring-probe --order rbcr` varies this sequence one call at a time.

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

The core API gives the same audio, so nothing is behind this door.

## A detail worth keeping

The Mac build ships **32 cores**, `SCCore00` through `SCCore31`, all exactly
1265.1 KB and all byte-different: the same code with a different embedded
constant, so each loads as a separate library with its own globals. The engine
is single-instance by design and Roland multi-instances it by shipping copies.
The Windows `SCCore.dll` is one 26.7 MB file carrying code and data together.

## Standing

Roland's binaries were read statically to answer two questions measurement
alone could not: which globals `TG_Process` loads before it copies, and what
`TG_activate` does with its second argument. Both are calling-convention and
allocation facts about a published C API, not algorithms, and neither is
reproduced beyond the few lines quoted here.

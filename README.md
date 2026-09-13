# scva-linux

Run Roland's **SOUND Canvas VA** engine on Linux, headless: as a one-shot
file renderer, or as a daemon that appears as an ordinary ALSA MIDI device
you can play into from anything.

Nothing of Roland's is included here. You supply the DLL.

## Supply the engine

Copy these out of your own SOUND Canvas VA installation into `dll/`, or set
`SCVA_DLL_DIR` to wherever you keep them:

    SCCore.dll
    parameter1.dat  parameter2.dat  parameter3.dat
    GM.drf GM.drk GM.tnf GM2.drk GM2.tnf SCVSC.drf SCVSC.drk SCVSC.tnf

`dll/` is gitignored and must stay that way.

## Build

    make                 # needs mingw-w64 for the engine, alsa-lib for the daemon

## Run it as a MIDI device

    ./build/scva-daemon

It creates an ALSA sequencer port called **SCVA**, then plays whatever you
send it:

    aconnect -l                  # find the port
    aconnect 24:0 'SCVA':0       # wire a keyboard or a player to it
    aplaymidi -p 'SCVA' song.mid

## Render a file instead

    ./build/scva-render --midi song.mid --out song.wav --rate 44100

## The one thing that makes it work

`TG_setSampleRate` must be called **twice**: once before
`TG_setMaxBlockSize`, and again as the **last call before `TG_activate`**.
Set it only once, on either side, and the core writes `+/-Inf` from the
second sample onward and `NaN` after that - the engine appears to
initialise perfectly, reports no error, and emits garbage.

Measured one variable at a time:

| sequence | result |
|---|---|
| `rate, block, rate` | finite, 2048 of 2048 frames |
| `rate, block, config, rate` | finite |
| `rate, block` | diverges |
| `rate, block, config` | diverges |
| `rate, block, rate, config` | diverges |
| `block, rate` | diverges |
| no rate call | diverges |

So `setMaxBlockSize` **and** `XPsetSystemConfig` both invalidate the
rate-derived state, and a rate call is still needed before
`setMaxBlockSize` - which is why `block, rate` alone fails.

The control that says the rate genuinely arrives, rather than the output
being accidentally finite: before the fix, sample 0 was bit-identical at
22050, 44100 and 96000 Hz. After it, it differs at each.

## Standing

This project treats the engine as a **black box**: it loads the DLL and
calls its exported entry points. No part of Roland's code is disassembled
here, and none of it is redistributed. Audio produced this way is Roland's
own plug-in output and is not a Roland SC-88 - it is a *proxy*, and should
be labelled as such wherever it is used as a reference.

Our own code here is CC0.

## Known defect: the right channel glitches

The left channel is clean and the right channel is not. On a single C4 piano
note the left has **zero** discontinuities and the right has **88**; on a
three-minute song the left has 346 and the right **23,026**. Novak's recording
of the same plug-in hosted in a DAW has **zero** in its right channel, so the
engine is capable of a clean one and the fault is in how this project drives it.

The artefact: left and right track each other exactly, then the right jumps to a
value several times larger and resyncs a few samples later. At one instance
`R[484:500]` equals `L[496:512]` **exactly** - the right channel momentarily
runs 12 samples ahead.

### Ruled out, each measured rather than argued

| hypothesis | how it was killed |
|---|---|
| clipping | peak 0.647, not one sample above 0.7 |
| output format | float32 WAV, so no quantisation grit |
| our interleaving | the divergence is present in the engine's own buffers before any file is written: `L[228] = -0.008251`, `R[228] = -0.035630` |
| buffer overrun | a sentinel fill shows `TG_Process` writes exactly `frames` samples to each buffer, no holes, no overrun |
| buffer layout | one contiguous planar allocation is byte-identical to two separate ones |
| process block size | BLOCK 64 and 256 are bit-identical; 1024 differs only by MIDI landing on another boundary |
| declared max block | 4096 and 256 are bit-identical |
| polyphony | 1, 4 and 16 simultaneous notes are all clean |
| a race | three runs are bit-identical, the same 88 glitches each time |
| rate call placement | a third `setSampleRate` after `activate` changes nothing |
| `TG_setInterruptThreadIdAtThisTime` | calling it from the render thread changes nothing |
| `TG_XPsetSystemConfig` | clamped to (1,1); 0 silences the engine, 2 and 3 clamp to 1, skipping it is identical |
| floating-point environment | flush-to-zero, denormals-are-zero and round-toward-zero all change nothing |

### The VST route, which is the live lead

Novak's clean right channel came through the plug-in hosted in a DAW, not
through the core directly - so the wrapper does something we do not.

`src/scva_vst_shim.c` gets that path open. The wrapper exports its entry as
**`R2RPluginMain`** rather than the `VSTPluginMain` a host looks for, so no host
can open it unaided; the shim publishes the expected name and forwards. It
resolves `Wrapper.dll` next to itself rather than through the registry, so the
host's working directory cannot change which one is found.

It replaces a third-party shim that ships alongside the wrapper and **fails its
own DllMain under wine** - `LoadLibrary` returns 1114, ERROR_DLL_INIT_FAILED -
while the wrapper itself loads perfectly. That shim also reads
`HKLM\SOFTWARE\Roland Cloud\SOUND Canvas VA`, and the prefix only carried the
key under `Wow6432Node`, the 32-bit view, which a 64-bit host does not read.
Adding it to the 64-bit hive did not revive it; ours needs no registry at all.

With our shim the plug-in opens and identifies itself, and stops at one
specific thing:

    Plugin 'scva' asked for directory pointer (unsupported)
    ERROR: Sent signal 11

It calls **`audioMasterGetDirectory`** to locate its data, MrsWatson answers
"unsupported", and the plug-in dereferences the null it gets back. So the
blocker is now a single host callback rather than anything unknown.

**Next**: a minimal VST2 host of our own that answers `audioMasterGetDirectory`
with the plug-in's directory and then drives `processReplacing`. That is a small
amount of code and it is the last thing between here and a clean-right-channel
oracle.

### Also still untried

- `TG_PMidiIn`, `TG_Pt` and `TG_flushMidi`, the three core exports nothing calls.
- Whether wine's own float handling differs from Windows in a way the FP-mode
  test above does not reach.

### A detail worth keeping

The Mac build ships **32 cores**, `SCCore00` through `SCCore31`, all exactly
1265.1 KB and all **byte-different**: the same code with a different embedded
constant, so each loads as a separate library with its own globals. The engine
is single-instance by design and Roland multi-instances it by shipping copies.
The Windows `SCCore.dll` is one 26.7 MB file carrying code and data together.

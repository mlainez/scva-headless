# scva-linux

Roland's **SOUND Canvas VA** engine, headless, on Linux: as an ALSA MIDI
device you can play into, or as a file renderer.

Nothing of Roland's is included here. You supply the DLL.

## Requirements

- a C compiler and `alsa-lib` for the native path
- `mingw-w64` for the Windows path (optional)
- `wine` only if you run the Windows binaries on Linux

## Supply the engine

Copy these out of your own SOUND Canvas VA installation into `dll/`:

    SCCore.dll
    parameter1.dat  parameter2.dat  parameter3.dat
    GM.drf GM.drk GM.tnf GM2.drk GM2.tnf SCVSC.drf SCVSC.drk SCVSC.tnf

`dll/` is gitignored and must stay that way.

Every program finds the core the same way: `--core`, else `$SCVA_DLL_DIR/SCCore.dll`,
else `dll/SCCore.dll`. Same on Linux and Windows.

## Build

    make linux      # native Linux binaries - no wine
    make windows    # Windows PE binaries, via mingw-w64
    make            # both
    make probes     # diagnostics

Both paths produce **byte-identical audio**, verified over a 5,840,640-frame
render. The Windows binaries run unchanged on Windows; wine is only how this
machine executes them.

## Run it as a MIDI device

    make linux windows        # the daemon drives the Windows engine binary
    ./build/scva-daemon

It creates an ALSA sequencer port called **SCVA** and plays what you send it:

    aconnect -l                  # find the port
    aconnect 24:0 'SCVA':0       # wire a keyboard or player to it
    aplaymidi -p 'SCVA' song.mid

Options:

    --name NAME      sequencer port name        (default SCVA)
    --pcm DEV        ALSA output device         (default "default")
    --rate HZ        sample rate                (default 44100)
    --block N        frames per render block    (default 256)
    --dll-dir DIR    where SCCore.dll lives     (default dll, or $SCVA_DLL_DIR)
    --engine EXE     engine binary              (default build/scva_engine.exe)

The PCM device sets the pace: writing to it blocks until the card has room,
which throttles the engine. There is no timer anywhere in the daemon.

## Render a MIDI file

Native, no wine:

    ./build/scva-native --midi song.mid --out song.wav

Or the Windows binary:

    wine build/scva_render.exe --midi song.mid --out song.wav

Options, the same for both:

    --midi FILE      input Standard MIDI File   (required)
    --out FILE       output float32 stereo WAV  (required)
    --core DLL       path to SCCore.dll
    --rate HZ        sample rate                (default 48000)
    --tail SECONDS   silence rendered after the last event (default 3)
    --reset gs|gm|none                          (default gs)
    --maxblock N     engine's declared maximum block (default 4096)
    --flat-out       render as fast as possible rather than pacing to
                     wall-clock time; the engine generates on demand, so this
                     is correct and much faster

Output is float32 stereo WAV. Convert with `ffmpeg -i song.wav out.flac` if you
want something smaller.

## Which path to use

The native one, unless you need a Windows build. It needs no wine, starts
faster, and produces the same bytes.

## Diagnostics

    make probes
    ./build/ring-probe        # ring buffers, sizes, init-order experiments
    ./build/overrun-probe     # what TG_Process writes, per channel
    python3 scripts/wavcmp.py stats a.wav [b.wav]
    python3 scripts/wavcmp.py head a.wav [n]

Everything reports **per channel**. A defect can live in one channel only, and
a detector that reports a single number will not see it.

## How the engine has to be driven

Several things are not guessable and all of them fail silently - no error, and
audio that sounds plausible. They are in
[docs/ENGINE-NOTES.md](docs/ENGINE-NOTES.md) with the measurements and the real
signatures:

- `TG_activate(float rate, int maxBlockSize)` - the first argument is a float
  in XMM0, and the second is the block size, not a flag. A small second
  argument reallocates the engine's ring buffers to one float each and
  corrupts the right channel.
- `TG_ShortMidiIn` and `TG_LongMidiIn` each take a **timestamp** as their
  second argument. `TG_LongMidiIn` does not take a length: the message is
  F7-terminated.
- A SysEx in a MIDI file is stored **without its leading F0** and has to be
  rebuilt, or the engine ignores it.
- `TG_setSampleRate` must be called **twice**, the second time as the last call
  before `TG_activate`, or the engine emits `Inf` and `NaN`.

## Which Sound Canvas is it?

The SC-8820 map, and that is not selectable. The product's own tone file names
four maps - 55Map, 88Map, 88ProMap, 8820Map - and they assign different sounds
to the same program and bank, so bank numbers cannot stand in for them.
Nothing `SCCore.dll` exports selects a map, and neither does the GS tone-map
SysEx. Bank Select (CC0) reaches the 1262 variations within the 8820 map,
which is a different axis.

## Standing

This project treats the engine as a black box in what it ships: it loads the
DLL and calls its exported entry points. None of Roland's code is
redistributed here.

Audio produced this way is Roland's own plug-in output and is **not** a Roland
SC-88 - it is a *proxy*, and should be labelled as such wherever it is used as
a reference.

Our own code here is CC0.

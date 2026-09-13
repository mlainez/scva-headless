# scva-linux

Roland's **SOUND Canvas VA** engine, headless, on Linux: as an ALSA MIDI
device, or as a file renderer.

Nothing of Roland's is included here. You supply the DLL.

## Requirements

- a C compiler and `alsa-lib` for the native path
- `mingw-w64` for the Windows path
- `wine` only to run the Windows binaries on Linux

## Supply the engine

Copy these out of your own SOUND Canvas VA installation into `dll/`:

    SCCore.dll
    parameter1.dat  parameter2.dat  parameter3.dat
    GM.drf GM.drk GM.tnf GM2.drk GM2.tnf SCVSC.drf SCVSC.drk SCVSC.tnf

`dll/` is gitignored and must stay that way.

The core is looked for as `--core`, then `$SCVA_DLL_DIR/SCCore.dll`, then
`dll/SCCore.dll`. Same on Linux and Windows.

## Build

    make linux      # native, no wine
    make windows    # Windows PE binaries
    make            # both

Both produce byte-identical audio, verified over a 5,840,640-frame render. The
Windows binaries run unchanged on Windows.

## Render a MIDI file

    ./build/scva-native --midi song.mid --out song.wav --map 88
    wine build/scva_render.exe --midi song.mid --out song.wav --map 88

    --midi FILE      input Standard MIDI File        (required)
    --out FILE       output float32 stereo WAV       (required)
    --map WHICH      default | 55 | 88 | 88pro | 8820   (default: default)
    --core DLL       path to SCCore.dll
    --rate HZ        sample rate                     (default 48000)
    --tail SECONDS   silence after the last event    (default 3)
    --reset gs|gm|none                               (default gs)
    --maxblock N     declared maximum block          (default 4096)
    --flat-out       render as fast as possible rather than in real time
    --init N         TG_initialize argument          (default 0)
    --cfg A B        TG_XPsetSystemConfig fields     (default 1 1)

## Tone maps

The engine is an **SC-8820** and holds four tone maps. They are not subsets of
one another: the same program and bank is a different sound in each.

    --map 55      SC-55       --map 88pro   SC-88Pro
    --map 88      SC-88       --map 8820    SC-8820
    --map default same as 8820

**The default is the SC-8820 map.** If you are using this as an SC-88 proxy,
pass `--map 88` or you are comparing against the wrong instrument.

The map is Bank Select LSB (CC32), sent on all 16 parts after the reset and
again after any GS reset in the file, because a GS reset clears it.

## Run it as a MIDI device

    make linux windows
    ./build/scva-daemon

Creates an ALSA sequencer port called **SCVA**:

    aconnect -l
    aconnect 24:0 'SCVA':0
    aplaymidi -p 'SCVA' song.mid

    --name NAME      port name                  (default SCVA)
    --pcm DEV        ALSA output device         (default "default")
    --rate HZ        sample rate                (default 44100)
    --block N        frames per block           (default 256)
    --dll-dir DIR    where SCCore.dll lives     (default dll, or $SCVA_DLL_DIR)
    --engine EXE     engine binary              (default build/scva_engine.exe)

The PCM device sets the pace; there is no timer in the daemon.

## Driving the engine

Several exported functions fail silently if called wrongly. The signatures and
the measurements are in [docs/ENGINE-NOTES.md](docs/ENGINE-NOTES.md).

## Standing

This project loads the DLL and calls its exported entry points. None of
Roland's code is redistributed here.

Audio produced this way is Roland's own plug-in output and is **not** a Roland
SC-88 - it is a *proxy*, and should be labelled as such.

Our own code here is CC0.

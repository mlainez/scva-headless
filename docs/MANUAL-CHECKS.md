# Manual checks

What the automated tiers (`tests/checkpoint.sh`) cannot reach: real Windows,
real MIDI hardware or loopback ports, realtime rights, and DAWs. Run the
relevant items when a change touches what they cover, and always before a
release. Each item has one pass criterion; record the result in the pull
request next to the checkpoint table.

## scva-winmidi on real Windows

wine's `midiIn` delivers nothing, so none of this runs in CI. Use
[loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) and any
sequencer or `sendmidi`.

| # | do | pass when | protects |
|---|---|---|---|
| W1 | `scva-winmidi --midi-in loopMIDI`, play a song | it plays, without stutter, at the default 40 ms | the live path |
| W2 | play a part with steady 16th notes | the timing is even, no drift over a minute | 2ec97f8, f5f0780: scheduling against the played position |
| W3 | send three different GS SysEx in a row (e.g. reverb macros) | all three take effect | 5d0cfea: SysEx buffers kept queued |
| W4 | open Task Manager, Details | `scva-winmidi.exe` priority is High | 2f30d7a |
| W5 | type `88`, then `10 55`, then Enter in its console | each change is heard at once; Enter shows the per-part maps | 059ea80, 7e7774d |
| W6 | send CC32 2 then a program change on channel 1 | the part stays on the console's map | e4a944e |
| W7 | two instances with `--name A` and `--name B` | two window titles, both play | cd3d299 |

## Windows 98

On real hardware (or 86Box), with a 32-bit core.

| # | do | pass when | protects |
|---|---|---|---|
| R1 | `scva-render32.exe --midi song.mid --out out.wav` | a WAV that plays | the Win98 renderer |
| R2 | `scva-render32.exe --midi song.mid --play` | it plays through the sound card | waveOut path |
| R3 | `scva-winmidi32.exe` with a MIDI input | it plays | the Win98 live path |
| R4 | on a CPU without SSE2 (Athlon XP, Pentium III) | R1 works and reports the SSE2 shim | 0209c9f |
| R5 | load `scva-win32.clap` in a Win98-capable host (e.g. SAVIHost) | it loads and plays | open: the DLL imports `GetModuleHandleW`, a stub on Win9x (known issue, see below) |

## Virtual Sound Canvas on Windows 98

On real hardware (or 86Box), with `vsc.sys` and a tone set from the VSC 3.2
CD in one directory.

| # | do | pass when | protects |
|---|---|---|---|
| V1 | `scva-render32.exe --engine vsc --core vsc.sys --midi song.mid --out out.wav` | a WAV that plays | the VSC engine under the Win98 loader |
| V2 | the same with `--play` | it plays through the sound card | waveOut with the engine's own block size |
| V3 | the same with `--cpu fpu` on a machine without MMX, if one is at hand | it renders | the engine's x87 path |
| V4 | `scva-winmidi32.exe --engine vsc --core vsc.sys` with a MIDI input, then type `55` | it plays, and the instruments change to the SC-55 set | the VSC engine live on Windows |

## scva-daemon with realtime rights

The tests check the refused path everywhere; the granted path needs a user
with rtprio (e.g. in the `audio` or `pipewire` group with limits set).

| # | do | pass when | protects |
|---|---|---|---|
| D1 | start `scva-daemon` | stderr says `realtime priority 6`, `chrt -p <pid>` shows SCHED_FIFO 6 | the realtime setup |
| D2 | play through a real sound card for 5 minutes | `0 dropouts` when stopping | latency at the default 20 ms |
| D3 | `scva-daemon32 --engine vsc --core vsc.sys`, play a song into it, type `55` | it plays, and the instruments change to the SC-55 set | the VSC engine live; its set changes |

## Plugins in a DAW

One host per format is enough (Carla, Reaper, Bitwig, Ardour...).

| # | do | pass when | protects |
|---|---|---|---|
| P1 | load the LV2, play a MIDI clip | sound; the editor opens | LV2 + X11 editor |
| P2 | change Tone Map while the clip plays | the instrument changes at once | 91de49c (CLAP), map ports |
| P3 | pick an Instrument and a Drum Kit in the editor | the parts change; the DAW's parameters follow | 1fd0fb3, editor writes |
| P4 | save, close and reopen the project | the same map, instrument and kit come back | parameter persistence |
| P5 | load the CLAP; browse its presets | every map's tones then kits, named `SC-88 001 Piano 1` | preset discovery |
| P6 | scale the display to 150% (Windows) and open the CLAP editor | the editor scales and fills the window | 2e59261 |
| P7 | reopen a saved CLAP project that used a preset | the preset's patch is what plays (see the known issue below) | preset restore |
| P8 | in a 32-bit host (Carla's 32-bit bridge, SAVIHost x86), load "SCVA: Virtual Sound Canvas 3.2", play, change Tone Set | sound at the host's rate; the editor lists the engine's names; the set change is heard | the VSC plugin, resampled |

## Known issues the automated tiers report

These print as "known issue" instead of failing, so they stay visible.

- **scva-win32.clap imports `GetModuleHandleW`**, from the mingw-w64 DLL
  startup code. It is a stub on Windows 98; R5 decides whether it matters.
- **A CLAP preset loaded before the first `process()` is overridden** by the
  Instrument parameter's own choice in that same call. A host restoring a
  session may do exactly that; P7 shows whether real hosts are affected.

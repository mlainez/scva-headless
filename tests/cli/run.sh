#!/bin/sh
# SPDX-License-Identifier: CC0-1.0
#
# Command-line behaviour of every binary, up to the point where a core would
# be needed: what users get for good and bad arguments, where each tool looks
# for SCCore.dll, the daemon's control client, and scva-tones' output.
# No core is present here; SCVA_DLL_DIR is cleared for every test.
. tests/cli/lib.sh
unset SCVA_DLL_DIR SCVA_TONES XDG_RUNTIME_DIR

# ---- scva-render, all four builds --------------------------------------
for r in scva-render scva-render32 scva-render.exe scva-render32.exe; do
  have "$r" || continue

  begin "$r: without arguments it explains what it needs"
  run "$r"
  exit_is 2; says --midi; says --out
  end

  begin "$r: a MIDI file alone is not enough, it needs somewhere to play"
  run "$r" --midi song.mid
  exit_is 2; says --out
  end

  begin "$r: --bits accepts only 16 or 32"
  run "$r" --midi song.mid --out out.wav --bits 24
  exit_is 2; says 16; says 32
  end

  begin "$r: an unknown --map is refused and the valid ones are listed"
  run "$r" --midi song.mid --out out.wav --map sc-8850
  exit_is 2; says 88pro; says 8820
  end

  begin "$r: an unknown option is a usage error"
  run "$r" --midi song.mid --out out.wav --frobnicate
  exit_is 2
  end

  begin "$r: with no core it fails and says it looked in dll/ beside it"
  run "$r" --midi song.mid --out out.wav
  case "$r" in
    *.exe) exit_is 1; says 'dll\SCCore.dll' ;;
    *)     exit_is 1; says 'dll/SCCore.dll' ;;
  esac
  end

  begin "$r: SCVA_DLL_DIR is where it looks"
  SCVA_DLL_DIR=/no/such/place; export SCVA_DLL_DIR
  run "$r" --midi song.mid --out out.wav
  unset SCVA_DLL_DIR
  exit_is 1; says /no/such/place; says SCCore.dll
  end

  begin "$r: --core names the core to load"
  run "$r" --midi song.mid --out out.wav --core /elsewhere/MyCore.dll
  exit_is 1; says /elsewhere/MyCore.dll
  end

  begin "$r: every documented map name is accepted"
  for m in default 55 88 88pro 8820 SC-88Pro; do
    run "$r" --midi song.mid --out out.wav --map "$m"
    exit_is 1                    # gets past the map, stops at the core
  done
  end
done

if have scva-render32; then
  begin "scva-render32: --play is refused, it is built without ALSA"
  run scva-render32 --midi song.mid --play
  exit_is 2; says --out
  end
fi

if have scva-render; then
  begin "scva-render: --play is a real output, it only stops at the missing core"
  run scva-render --midi song.mid --play
  exit_is 1; says SCCore.dll
  end
fi

# ---- --engine, every renderer -------------------------------------------
for r in scva-render scva-render32 scva-render.exe scva-render32.exe; do
  have "$r" || continue

  begin "$r: an unknown --engine is refused and the ones it has are named"
  run "$r" --midi song.mid --out out.wav --engine nonesuch
  exit_is 2; says scva
  end

  case "$r" in
    *32*) ;;
    *)
      begin "$r: a 64-bit build has no VSC engine, whose code is 32-bit"
      run "$r" --midi song.mid --out out.wav --engine vsc
      exit_is 2; says scva
      end
      continue ;;
  esac

  begin "$r: its usage shows --engine and the VSC engine's own options"
  run "$r" --frobnicate
  exit_is 2; says --engine; says vsc; says --set; says --poly; says --volume
  says --rev-gain; says --cpu
  end

  begin "$r: --engine vsc needs its engine file"
  run "$r" --engine vsc --midi song.mid --out out.wav
  exit_is 2; says --core
  end

  begin "$r: --engine vsc refuses a rate VSC lacks and names the ones it has"
  run "$r" --engine vsc --core vsc.sys --midi song.mid --out out.wav --rate 48000
  exit_is 2; says 11025; says 22050; says 44100
  end

  begin "$r: --engine vsc checks its own options' values"
  run "$r" --engine vsc --core vsc.sys --midi song.mid --out out.wav --set 99
  exit_is 2; says 55; says 88pro
  run "$r" --engine vsc --core vsc.sys --midi song.mid --out out.wav --poly 0
  exit_is 2; says 128
  run "$r" --engine vsc --core vsc.sys --midi song.mid --out out.wav --cpu sse9
  exit_is 2; says mmx
  end

  begin "$r: --engine vsc takes its tone set from --set, not --map"
  run "$r" --engine vsc --core vsc.sys --midi song.mid --out out.wav --map 88
  exit_is 2; says --set
  end

  begin "$r: the VSC engine's options mean nothing to the default engine"
  run "$r" --midi song.mid --out out.wav --set 88
  exit_is 2
  end
done

# ---- scva-daemon, both widths -------------------------------------------
for d in scva-daemon scva-daemon32; do
  have "$d" || continue

  begin "$d: --help shows the options and succeeds"
  run "$d" --help
  exit_is 0; says --send; says --list-pcm; says --control
  end

  begin "$d: -h is --help"
  run "$d" -h
  exit_is 0; says --send
  end

  begin "$d: an unknown option is a usage error"
  run "$d" --frobnicate
  exit_is 2; says --send
  end

  begin "$d: an unknown --map is refused and the valid ones are listed"
  run "$d" --map sc-8850
  exit_is 2; says 88pro
  end

  begin "$d: --list-pcm lists the devices it would try, default first"
  run "$d" --list-pcm
  exit_is 0
  [ "$(sed -n 2p "$out" | awk '{print $1}')" = default ] || no "default as the first candidate"
  end

  begin "$d: with no core it fails and says how to point at one"
  run "$d"
  exit_is 1; says SCCore.dll; says --core; says SCVA_DLL_DIR
  end

  begin "$d: a block below the engine's minimum is raised to 256, with a warning"
  run "$d" --block 100
  says 100; says 256
  end

  begin "$d: --send with no daemon running fails and names the socket"
  XDG_RUNTIME_DIR=$scratch/run; export XDG_RUNTIME_DIR
  run "$d" --send 88
  unset XDG_RUNTIME_DIR
  exit_is 1; says "$scratch/run/scva-daemon-SCVA.sock"
  end

  begin "$d: the socket is named after --name, in any argument order"
  XDG_RUNTIME_DIR=$scratch/run; export XDG_RUNTIME_DIR
  run "$d" --send 88 --name studio/a
  exit_is 1; says "$scratch/run/scva-daemon-studio_a.sock"
  run "$d" --name studio/a --send 88
  says "$scratch/run/scva-daemon-studio_a.sock"
  unset XDG_RUNTIME_DIR
  end

  begin "$d: without XDG_RUNTIME_DIR the socket is per user in /tmp"
  run "$d" --send 88 --name B
  exit_is 1; says "/tmp/scva-daemon-B-$(id -u).sock"
  end

  begin "$d: --control overrides the socket path"
  run "$d" --send 88 --control "$scratch/mine.sock"
  exit_is 1; says "$scratch/mine.sock"
  end

  if have_tool python3 "$d: --send talks to a running daemon"; then
    begin "$d: --send delivers one command line and prints the daemon's reply"
    sock=$scratch/ctl.sock
    python3 tests/cli/fake_control.py "$sock" "$scratch/received" "channel 10 -> map 3
" &
    fake=$!
    i=0; while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
    run "$d" --send "10 88pro" --control "$sock"
    wait $fake
    exit_is 0
    stdout_has "channel 10 -> map 3"
    [ "$(cat "$scratch/received")" = "10 88pro" ] || no "the daemon to receive exactly '10 88pro'"
    [ "$(tail -c 1 "$scratch/received" | od -An -c | tr -d ' ')" = '\n' ] || no "the command to end with a newline"
    end
  fi
done

# ---- scva-winmidi, both widths -------------------------------------------
for w in scva-winmidi.exe scva-winmidi32.exe; do
  have "$w" || continue

  begin "$w: --help shows the options and succeeds"
  run "$w" --help
  exit_is 0; says --midi-in; says --list
  end

  begin "$w: an unknown option is a usage error"
  run "$w" --frobnicate
  exit_is 2
  end

  begin "$w: an unknown --map is refused"
  run "$w" --map sc-8850
  exit_is 2; says 88pro
  end

  begin "$w: --list succeeds without a core"
  run "$w" --list
  exit_is 0
  end

  begin "$w: with no core it fails and says how to point at one"
  run "$w"
  exit_is 1; says SCCore.dll; says SCVA_DLL_DIR
  end

  begin "$w: a block below the engine's minimum is raised to 256, with a warning"
  run "$w" --block 100
  says 100; says 256
  end
done

# ---- --engine in the live tools ------------------------------------------
for t in scva-daemon scva-daemon32 scva-winmidi.exe scva-winmidi32.exe; do
  have "$t" || continue

  begin "$t: an unknown --engine is refused and the ones it has are named"
  run "$t" --engine nonesuch
  exit_is 2; says scva
  end

  case "$t" in
    *32*)
      begin "$t: its help shows --engine and the VSC engine's own options"
      run "$t" --help
      exit_is 0; says --engine; says vsc; says --set; says --poly
      end

      begin "$t: --engine vsc needs its engine file"
      run "$t" --engine vsc
      exit_is 2; says --core
      end

      begin "$t: --engine vsc checks its own options' values"
      run "$t" --engine vsc --core vsc.sys --volume 101
      exit_is 2; says 100
      end
      ;;
  esac
done

# ---- scva-tones, both widths ----------------------------------------------
synthetic=$scratch/names
mkdir -p "$synthetic"
printf 'MODULENAME=SC-8820\r\nPAGE=2\t88Map\r\n\t0\t8\r\n1\tPiano 1\tPiano 1w\r\n' >"$synthetic/SCVSC.tnf"
printf 'PAGE=2\t88Map\r\n\t0\r\n1\tSTANDARD 1\r\n' >"$synthetic/SCVSC.drf"
printf 'PAGE=2\t88Map\r\n\t0\r\n36\tKick Drum 1\r\n' >"$synthetic/SCVSC.drk"

for t in scva-tones scva-tones32; do
  have "$t" || continue

  begin "$t: --help succeeds"
  run "$t" --help
  exit_is 0; says --core
  end

  begin "$t: an unknown argument is refused"
  run "$t" --frobnicate
  exit_is 2; says --frobnicate
  end

  begin "$t: with no name files it writes the General MIDI set"
  mkdir -p "$scratch/empty"
  run "$t" --core "$scratch/empty"
  exit_is 0
  stdout_has "General MIDI"
  [ "$(grep -c '^t ' "$out")" = 128 ] || no "128 melodic tones"
  [ "$(grep -c '^d ' "$out")" = 1 ] || no "one drum kit"
  [ "$(grep -c '^k ' "$out")" = 47 ] || no "47 drum keys"
  stdout_has "t 0 0 0 Acoustic Grand Piano"
  end

  begin "$t: it writes every tone, kit and key the name files hold"
  run "$t" --core "$synthetic"
  exit_is 0
  stdout_has "t 2 0 0 Piano 1"
  stdout_has "t 2 8 0 Piano 1w"
  stdout_has "d 2 0 0 STANDARD 1"
  stdout_has "k 2 0 36 Kick Drum 1"
  end

  begin "$t: SCVA_DLL_DIR is where it reads when --core is not given"
  SCVA_DLL_DIR=$synthetic; export SCVA_DLL_DIR
  run "$t"
  unset SCVA_DLL_DIR
  exit_is 0; stdout_has "t 2 8 0 Piano 1w"
  end

  begin "$t: its output read back through SCVA_TONES gives the same table"
  run "$t" --core "$synthetic"
  grep -v '^#' "$out" >"$scratch/first"
  SCVA_TONES=$scratch/first; export SCVA_TONES
  run "$t" --core "$scratch/empty"
  unset SCVA_TONES
  exit_is 0
  grep -v '^#' "$out" | diff -q - "$scratch/first" >/dev/null || no "the same records"
  stdout_has "module: custom"
  end
done

# ---- what the built files promise --------------------------------------
# Properties users depend on that no run here can observe: which Windows
# will load a binary, and what a plugin exposes to its host.

pe_objdump() {
  case "$1" in
    *32*) echo i686-w64-mingw32-objdump ;;
    *)    echo x86_64-w64-mingw32-objdump ;;
  esac
}

pe_field()   { $(pe_objdump "$1") -p "$BUILD/$1" | awk -v k="$2" '$1 == k {print $2}'; }
pe_imports() { $(pe_objdump "$1") -p "$BUILD/$1" | sed -n 's/^\t\{0,1\}[ 0-9a-f]*[ \t]\{1,\}[0-9]\{1,\}[ \t]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\)$/\1/p'; }
pe_dlls()    { $(pe_objdump "$1") -p "$BUILD/$1" | sed -n 's/^\tDLL Name: //p' | tr 'A-Z' 'a-z' | sort -u | tr '\n' ' '; }

if have_tool i686-w64-mingw32-objdump "Windows 98 artifact checks"; then
  for f in scva-render32.exe scva-winmidi32.exe scva-win32.clap; do
    have "$f" || continue
    begin "$f: Windows 98 will load it (subsystem 4.0)"
    [ "$(pe_field "$f" MajorSubsystemVersion).$(pe_field "$f" MinorSubsystemVersion)" = 4.0 ] \
      || no "subsystem 4.0"
    end

    begin "$f: it needs nothing Windows 98 lacks"
    dlls=$(pe_dlls "$f")
    for want in $dlls; do
      case "$want" in
        kernel32.dll|msvcrt.dll|winmm.dll|user32.dll|gdi32.dll) ;;
        *) no "only Windows 98 system DLLs, found $want" ;;
      esac
    done
    # These exist on Windows 98 only as stubs that fail.
    for stub in GetModuleHandleW MultiByteToWideChar WideCharToMultiByte; do
      pe_imports "$f" | grep -qx "$stub" || continue
      # Known: mingw-w64's DLL startup code imports GetModuleHandleW into
      # every DLL it links; this project's own code does not call it.
      # Whether Windows 98 minds is an open question (docs/MANUAL-CHECKS).
      if [ "$f:$stub" = scva-win32.clap:GetModuleHandleW ]; then
        echo "     known issue: $f imports $stub (from the mingw-w64 DLL startup code)"
        known_win98_stub=1
        continue
      fi
      no "no import of $stub"
    done
    end
    if [ "$f" = scva-win32.clap ] && [ -z "${known_win98_stub:-}" ]; then
      echo "     note: the known GetModuleHandleW import is gone; remove its exception"
    fi
  done
fi

if have_tool x86_64-w64-mingw32-objdump "Windows x64 artifact checks"; then
  for f in scva-render.exe scva-winmidi.exe; do
    have "$f" || continue
    begin "$f: it targets Windows XP x64 (subsystem 5.2)"
    [ "$(pe_field "$f" MajorSubsystemVersion).$(pe_field "$f" MinorSubsystemVersion)" = 5.2 ] \
      || no "subsystem 5.2"
    end
  done
fi

# Tripwires for scva-winmidi's timing work (2f30d7a, 5d0cfea, f5f0780): the
# behaviour itself needs real Windows and a MIDI port (see MANUAL-CHECKS),
# but losing any of these calls loses it.
for f in scva-winmidi.exe scva-winmidi32.exe; do
  have "$f" || continue
  have_tool "$(pe_objdump "$f")" "$f imports" || continue
  begin "$f: it raises its priority, sets 1 ms timers and re-queues SysEx buffers"
  for want in SetPriorityClass SetThreadPriority timeBeginPeriod midiInAddBuffer waveOutGetPosition; do
    pe_imports "$f" | grep -qx "$want" || no "an import of $want"
  done
  end
done

# Same for scva-daemon's realtime setup: the behaviour is checked with a core
# (test-fake), this catches the calls going missing in any build.
for f in scva-daemon scva-daemon32; do
  have "$f" || continue
  begin "$f: it asks for realtime scheduling and locked memory"
  for want in sched_setscheduler mlockall; do
    nm -D --undefined-only "$BUILD/$f" | grep -q " $want@" || no "a call to $want"
  done
  end
done

# A plugin that exports more than its entry point can collide with symbols in
# the host or in other plugins.
for f in scva.clap scva32.clap scva.lv2/scva.so scva.lv2/scva_ui.so \
         scva32.lv2/scva.so scva32.lv2/scva_ui.so; do
  have "$f" || continue
  case "$f" in
    *.clap)       entry=clap_entry ;;
    *scva_ui.so)  entry=lv2ui_descriptor ;;
    *)            entry=lv2_descriptor ;;
  esac
  begin "$f: it exports only $entry"
  exported=$(nm -D --defined-only "$BUILD/$f" | awk '$2 ~ /^[TDBR]$/ {print $3}' | grep -v '^_' | sort -u | tr '\n' ' ')
  [ "$exported" = "$entry " ] || no "only $entry, found: $exported"
  end
done

for f in scva-win64.clap scva-win32.clap; do
  have "$f" || continue
  have_tool "$(pe_objdump "$f")" "$f exports" || continue
  begin "$f: it exports only clap_entry"
  exported=$($(pe_objdump "$f") -p "$BUILD/$f" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' | awk 'NF >= 2 {print $NF}' | grep -v Table | tr '\n' ' ')
  [ "$exported" = "clap_entry " ] || no "only clap_entry, found: $exported"
  end
done

for b in scva.lv2 scva32.lv2; do
  have "$b/manifest.ttl" || continue
  begin "$b: the manifest names binaries that are in the bundle"
  for so in $(grep -o '<[a-z_]*\.so>' "$BUILD/$b/manifest.ttl" | tr -d '<>' | sort -u); do
    [ -e "$BUILD/$b/$so" ] || no "$so in the bundle"
  done
  end
  if command -v sord_validate >/dev/null 2>&1; then
    begin "$b: its Turtle is well-formed"
    sord_validate "$BUILD/$b/manifest.ttl" "$BUILD/$b/scva.ttl" >/dev/null 2>&1 || no "valid Turtle"
    end
  fi
done

summary

#!/bin/sh
# SPDX-License-Identifier: CC0-1.0
#
# The checkpoint for a change: builds every target CI builds, then runs every
# test tier this machine has what it needs for, and prints one table. Paste
# the table into the pull request.
#
#   CLAP_DIR=...           CLAP SDK include directory (as for make clap)
#   SCVA_TEST_DLL_DIR=...  the user's own SCCore.dll and name files, for the
#                          tiers that need them
#   SCVA_TEST_VSC_SYS=...  the user's own vsc.sys, for the loader's unit test
#                          that maps it
#
# Exit status is non-zero if any tier that ran failed.
set -u
cd "$(dirname "$0")/.." || exit 2
log=$(mktemp -d)
trap 'rm -rf "$log"' EXIT
results=""
failed=0

record() { results="$results$(printf '%-12s %-5s %s' "$1" "$2" "$3")
"; }

tier() {   # name, description, command...
  name=$1 desc=$2; shift 2
  if "$@" >"$log/$name" 2>&1; then
    record "$name" PASS "$desc"
  else
    record "$name" FAIL "$desc (see below)"
    failed=1
    echo "---- $name ----" >>"$log/failures"
    tail -40 "$log/$name" >>"$log/failures"
  fi
}

skip() { record "$1" SKIP "$2"; }

have() { command -v "$1" >/dev/null 2>&1; }

clap_dir=${CLAP_DIR:-/usr/include}
tier build "every CI target builds" \
  make linux linux32 daemon32 lv2 lv2-32 windows windows32 tones tones32 \
       clap clap-32 clap-win32 CLAP_DIR="$clap_dir"

tier unit "module behaviour, 64- and 32-bit" make test-unit

if have wine; then
  tier unit-win "module behaviour in the Windows builds, under wine" make test-unit-win
else
  skip unit-win "wine not installed"
fi

if have wine && have x86_64-w64-mingw32-objdump; then
  tier cli "command lines and artifacts of every binary" \
    env SCVA_TEST_STRICT=1 make test-cli
else
  tier cli "command lines and artifacts (no wine: Windows parts skipped)" make test-cli
fi

if have wine && have python3; then
  tier fake "every tool and plugin against the fake core (daemon needs /dev/snd/seq)" \
    env SCVA_TEST_STRICT=1 make test-fake CLAP_DIR="$clap_dir"
else
  skip fake "fake-core engine tests (need wine and python3)"
fi
if [ -n "${SCVA_TEST_DLL_DIR:-}" ] && [ -f "$SCVA_TEST_DLL_DIR/SCCore.dll" ]; then
  tier core "renders with your SCCore.dll match the baseline's audio" make test-core
else
  skip core "real-core golden renders (set SCVA_TEST_DLL_DIR)"
fi
if [ -n "${SCVA_TEST_DLL_DIR:-}" ] && [ -f "$SCVA_TEST_DLL_DIR/SCCore.dll" ]; then
  tier perf "instructions retired within 0.25% of the baseline" make test-perf
else
  skip perf "instruction-count budget (set SCVA_TEST_DLL_DIR)"
fi

echo
echo "checkpoint at $(git rev-parse --short HEAD 2>/dev/null || echo '?')$(git diff --quiet 2>/dev/null || echo ' + uncommitted changes')"
printf '%s' "$results"
[ -f "$log/failures" ] && { echo; cat "$log/failures"; }
exit $failed

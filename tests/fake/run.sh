#!/bin/sh
# SPDX-License-Identifier: CC0-1.0
#
# The engine-level tier: every tool against the fake core.
set -u
midi=$(mktemp -d)
trap 'rm -rf "$midi"' EXIT
python3 tests/midi/gen.py "$midi" || exit 2
export SCVA_TEST_MIDI=$midi
status=0
for t in tests/fake/test_*.py; do
  echo "== $t"
  python3 "$t" || status=1
done
exit $status

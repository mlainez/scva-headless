#!/bin/sh
# SPDX-License-Identifier: CC0-1.0
#
# The real-core tier; see tests/core/run.py. Pass --capture to record the
# goldens for this core on the baseline.
set -u
midi=$(mktemp -d)
trap 'rm -rf "$midi"' EXIT
python3 tests/midi/gen.py "$midi" || exit 2
SCVA_TEST_MIDI=$midi exec python3 tests/core/run.py "$@"

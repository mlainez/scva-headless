#!/bin/sh
# Runs each unit test binary given, in its own scratch directory, and fails if
# any of them does. SCVA_TEST_RUNNER=wine runs Windows builds under wine, in a
# prefix of their own so the user's is never touched.
set -u
status=0
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
runner=${SCVA_TEST_RUNNER:-}
if [ "$runner" = wine ]; then
  export WINEPREFIX="${SCVA_TEST_WINEPREFIX:-$PWD/build/test/wineprefix}"
  export WINEDEBUG=-all
  mkdir -p "$WINEPREFIX"
fi
for t in "$@"; do
  echo "== $t"
  TMPDIR=$scratch $runner "$t" || status=1
done
exit $status

# SPDX-License-Identifier: CC0-1.0
#
# Helpers for command-line behaviour tests. A test runs a binary, then states
# what a user would observe: the exit code and what the messages tell them.
# Messages are matched on the facts they carry (an option name, a path), not
# on their wording.
#
#   SCVA_TEST_STRICT=1   a binary or tool that is missing fails the run
#                        instead of being skipped (CI)

BUILD=${BUILD:-build}
pass=0 fail=0 skip=0
out=$(mktemp) err=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -rf "$out" "$err" "$scratch"' EXIT
current=""

export WINEDEBUG=-all
export WINEPREFIX="${SCVA_TEST_WINEPREFIX:-$PWD/$BUILD/test/wineprefix}"

# begin NAME: start a test case
begin() { current=$1; case_failed=0; }

# end: report the case
end() {
  if [ "$case_failed" = 0 ]; then echo "ok   $current"; pass=$((pass + 1))
  else echo "FAIL $current"; fail=$((fail + 1)); fi
}

skipped() { echo "skip $1 ($2)"; skip=$((skip + 1)); }

# have BINARY: true if built; otherwise skip (or fail when strict)
have() {
  if [ -e "$BUILD/$1" ]; then return 0; fi
  if [ "${SCVA_TEST_STRICT:-0}" = 1 ]; then
    echo "FAIL $1 is not built"; fail=$((fail + 1))
  else
    skipped "$1" "not built"
  fi
  return 1
}

have_tool() {
  if command -v "$1" >/dev/null 2>&1; then return 0; fi
  if [ "${SCVA_TEST_STRICT:-0}" = 1 ]; then
    echo "FAIL tool $1 is missing"; fail=$((fail + 1))
  else
    skipped "$2" "$1 not installed"
  fi
  return 1
}

# run BINARY ARGS...: run from an empty directory, with no core anywhere
# (unless the test exported SCVA_DLL_DIR itself). .exe files go through wine.
run() {
  bin=$PWD/$BUILD/$1; shift
  case "$bin" in
    *.exe) (cd "$scratch" && wine "$bin" "$@") >"$out" 2>"$err" </dev/null ;;
    *)     (cd "$scratch" && "$bin" "$@") >"$out" 2>"$err" </dev/null ;;
  esac
  status=$?
  # wine writes CRLF; compare on LF
  sed -i 's/\r$//' "$out" "$err"
}

no() {
  echo "    expected $1"
  sed 's/^/      out: /' "$out" | head -5
  sed 's/^/      err: /' "$err" | head -5
  case_failed=1
}

exit_is()      { [ "$status" = "$1" ] || no "exit $1, got $status"; }
says()         { cat "$out" "$err" | grep -qF -- "$1" || no "a message mentioning '$1'"; }
silent_on()    { cat "$out" "$err" | grep -qF -- "$1" && no "no mention of '$1'"; }
stdout_has()   { grep -qF -- "$1" "$out" || no "stdout containing '$1'"; }

summary() {
  echo "$pass passed, $fail failed, $skip skipped"
  [ "$fail" = 0 ]
}

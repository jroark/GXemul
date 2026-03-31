#!/bin/sh
#
#  MIPS16 test suite runner for GXemul.
#
#  Usage: ./test/mips16/run_tests.sh [path-to-gxemul]
#

GXEMUL="${1:-./gxemul}"
TESTDIR="$(dirname "$0")"
PASS=0
FAIL=0
ERRORS=""
TMPFILE=$(mktemp)

if [ ! -x "$GXEMUL" ]; then
    echo "ERROR: gxemul not found at $GXEMUL"
    exit 1
fi

run_test() {
    name="$1"
    binary="$2"
    expected="$3"

    if [ ! -f "$binary" ]; then
        echo "SKIP  $name (binary not found)"
        return
    fi

    "$GXEMUL" -q -E testmips "$binary" > "$TMPFILE" 2>/dev/null
    # Strip non-printable bytes (IC cache artifacts on shared pages)
    actual=$(LC_ALL=C sed 's/[^[:print:]]//g' "$TMPFILE")

    if [ "$actual" = "$expected" ]; then
        echo "PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $name"
        echo "  expected: '$expected'"
        echo "  actual:   '$actual'"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS $name"
    fi
}

echo "=== MIPS16 Test Suite ==="
echo ""

run_test "mode-switch"  "$TESTDIR/test_mips16"        "M32:M16:OK"
run_test "alu"          "$TESTDIR/test_mips16_alu"     "ABCDEFGHIJKLMNOPQRST"
run_test "memory"       "$TESTDIR/test_mips16_mem"     "ABCDEFGH"
run_test "branch-jump"  "$TESTDIR/test_mips16_branch"  "ABCDEFG"

rm -f "$TMPFILE"

echo ""
echo "--- Results: $PASS passed, $FAIL failed ---"

if [ $FAIL -gt 0 ]; then
    echo "Failed tests:$ERRORS"
    exit 1
fi

exit 0

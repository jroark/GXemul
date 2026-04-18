#!/bin/sh
#
#  MIPS16 test suite runner for GXemul.
#
#  Usage: ./test/mips16/run_tests.sh [path-to-gxemul]
#

TESTDIR="$(cd "$(dirname "$0")" && pwd)"
GXEMUL="${1:-$TESTDIR/../../gxemul}"
PASS=0
FAIL=0
ERRORS=""
TMPFILE=$(mktemp)

if [ ! -x "$GXEMUL" ] || [ -d "$GXEMUL" ]; then
    echo "ERROR: gxemul not found at $GXEMUL"
    exit 1
fi

run_test() {
    name="$1"
    binary="$2"
    expected="$3"

    if [ ! -f "$binary" ]; then
        echo "FAIL  $name"
        echo "  missing binary: '$binary'"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS $name"
        return
    fi

    "$GXEMUL" -q -E testmips "$binary" < /dev/null > "$TMPFILE" 2>/dev/null
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

while IFS='|' read -r name binary expected; do
    [ -n "$name" ] || continue
    run_test "$name" "$TESTDIR/$binary" "$expected"
done <<'EOF'
mode-switch|test_mips16|M32:M16:OK
alu|test_mips16_alu|ABCDEFGHIJKLMNOPQRST
memory|test_mips16_mem|ABCDEFGH
branch-jump|test_mips16_branch|ABCDEFG
muldiv|test_mips16_muldiv|ABCDEF
cross-mode|test_mips16_xmode|ABCDEFGH
extend|test_mips16_extend|ABCDEFGH
exception|test_mips16_except|ABCD
tlb-refill|test_mips16_tlb|ABCD
rria-extend|test_mips16_rria|ABCDEF
branch-extend|test_mips16_branch_ext|ABCDE
jrc-compact|test_mips16_jrc|AB
EOF

rm -f "$TMPFILE"

echo ""
echo "--- Results: $PASS passed, $FAIL failed ---"

if [ $FAIL -gt 0 ]; then
    echo "Failed tests:$ERRORS"
    exit 1
fi

exit 0

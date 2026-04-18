#!/bin/sh
#
#  Build MIPS16 test binaries using Docker cross-toolchain.
#
#  Requires: docker image 'mips-cross-dev' with mipsel-linux-gnu-gcc
#
#  Usage: ./test/mips16/build_tests.sh
#

TESTDIR="$(cd "$(dirname "$0")" && pwd)"
PROJDIR="$(cd "$TESTDIR/../.." && pwd)"

echo "Building MIPS16 test binaries..."

docker run --rm -v "$PROJDIR:/work" mips-cross-dev bash -c '
set -e
cd /work/test/mips16

for src in $(printf "%s\n" *.S | LC_ALL=C sort); do
    bin="${src%.S}"
    echo "  $src -> $bin"
    mipsel-linux-gnu-gcc -EB -mips32r2 -nostdlib \
        -Ttext 0xa0000000 -Wl,-e,_start \
        -o "$bin" "$src"
done

echo "Done."
'

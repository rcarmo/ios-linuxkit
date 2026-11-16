#!/bin/sh
# Link the actual core; use independent BUILD_DIR/EVIDENCE_DIR for each lane.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/build-arm64-linux}
OUT=${EVIDENCE_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/ish-block-hash.XXXXXX")}
CC=${CC:-clang}
mkdir -p "$OUT"
flags="-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1"
if grep -q -- '-DISH_JIT' "$BUILD/compile_commands.json"; then flags="$flags -DISH_JIT"; fi
san=""
if [ "${SANITIZE:-0}" = 1 ]; then san="-fsanitize=address,undefined -fno-omit-frame-pointer -latomic"; fi
$CC -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror $flags $san \
  -I"$ROOT" -I"$BUILD" "$ROOT/tests/arm64/upstream/block-hash.c" \
  -Wl,--start-group "$BUILD/libish.a" "$BUILD/libish_emu.a" "$BUILD/libfakefs.a" \
  -Wl,--end-group -lsqlite3 -lpthread -lm -ldl -lrt -o "$OUT/test"
timeout 20 "$OUT/test" > "$OUT/test.out" 2> "$OUT/test.err"
grep -qx block-hash-ok "$OUT/test.out"
echo "block-hash-test-ok $OUT"

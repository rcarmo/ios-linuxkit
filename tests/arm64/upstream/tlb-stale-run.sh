#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT/build-arm64-native-release}"
CC="${CC:-clang}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -DISH_JIT=1 -I"$PROJECT" -I"$BUILD_DIR" -pthread)
LIBS=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
for opt in -O0 -O2; do
    "$CC" "$opt" -Wall -Wextra -Werror "${DEFS[@]}" "$HERE/tlb-stale.c" "${LIBS[@]}" -o "$TMP/lifecycle"
    timeout 20 "$TMP/lifecycle"
    "$CC" "$opt" "${DEFS[@]}" -ffunction-sections -fdata-sections -Wl,--gc-sections \
        "$HERE/tlb-stale-cleanup.c" "${LIBS[@]}" -o "$TMP/cleanup"
    timeout 20 "$TMP/cleanup"
    "$CC" "$opt" "${DEFS[@]}" -ffunction-sections -fdata-sections -Wno-unused-function -Wl,--gc-sections \
        "$HERE/../native-aot/tlb-stale.c" "$HERE/../native-aot/call.S" \
        "$HERE/../native-aot/tlb-stale-gadgets.S" -o "$TMP/emitter"
    timeout 20 "$TMP/emitter"
done
# Preserve the existing real gadget CoW writeback and accounting safeguards.
"$CC" -O2 -Wall -Wextra -Werror "${DEFS[@]}" "$HERE/cow-store.c" \
    -Wl,--wrap=pthread_rwlock_trywrlock "${LIBS[@]}" -o "$TMP/cow-store"
timeout 20 "$TMP/cow-store"
ISH_JIT=0 timeout 20 "$TMP/cow-store"
# Offset generation must agree with the layout used by emitted LDUR.
grep -E 'define TLB_(stale|entries|segfault_addr)' "$BUILD_DIR/cpu-offsets.h"

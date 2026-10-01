#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT/build-memory-policy-release}"
CC="${CC:-clang}"
OUT="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$OUT"
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$OUT"' EXIT; fi
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
"$CC" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/cluster-span.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=munmap \
    -Wl,--wrap=pthread_rwlock_trywrlock -Wl,--start-group "$BUILD_DIR/libish.a" \
    "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group \
    -lrt -lm -ldl -lsqlite3 -o "$OUT/cluster-span"
checks=0
for span in 4096 16384; do
    for mode in basic boundary hole fork protect disabled no-predicate existing tlb \
            lazy lazy-fail lazy-none race-remove race-protect concurrent stack stack-mid cap pressure \
            gpf-lazy gpf-stack; do
        log="$OUT/$span-$mode.log"
        if timeout -k 2 15 "$OUT/cluster-span" "$span" "$mode" > "$log" 2>&1; then
            echo 0 > "$OUT/$span-$mode.rc"
        else
            rc=$?; echo "$rc" > "$OUT/$span-$mode.rc"; cat "$log"; exit "$rc"
        fi
        grep -q "local-real-cluster-ok span=$span mode=$mode" "$log"
        checks=$((checks + 1))
    done
done
for mode in mmap mmap-double data data-double; do
    log="$OUT/16384-$mode.log"
    if timeout -k 2 15 "$OUT/cluster-span" 16384 "$mode" > "$log" 2>&1; then
        echo 0 > "$OUT/16384-$mode.rc"
    else
        rc=$?; echo "$rc" > "$OUT/16384-$mode.rc"; cat "$log"; exit "$rc"
    fi
    grep -q "local-real-cluster-ok span=16384 mode=$mode" "$log"
    checks=$((checks + 1))
done
echo "local-cluster-gate-ok: $checks actual-kernel cases; synthetic spans, not Apple proof"

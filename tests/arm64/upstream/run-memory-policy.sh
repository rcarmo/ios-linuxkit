#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT/build-arm64-linux}"
CC="${CC:-clang}"
TMP="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$TMP"
if [[ -z "${EVIDENCE_DIR:-}" ]]; then trap 'rm -rf "$TMP"' EXIT; fi
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
LIBS=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
FLAGS=(-O2 -Wall -Wextra -Werror "${DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread)
# SANITIZE requires archives built with the same sanitizer/compiler as the tests.
if [[ "${SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    LIBS+=(-latomic)
fi
"$CC" "${FLAGS[@]}" "$HERE/protection-admission.c" -Wl,--wrap=mprotect -Wl,--wrap=malloc "${LIBS[@]}" -o "$TMP/protection-admission"
for mode in cap partial first reservation; do
    timeout 20 "$TMP/protection-admission" "$mode" > "$TMP/protection-$mode.log" 2>&1
    grep -q 'protection-admission-ok' "$TMP/protection-$mode.log"
done
"$CC" "${FLAGS[@]}" "$HERE/memory-policy.c" -Wl,--wrap=clock_gettime -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=mprotect -Wl,--wrap=munmap "${LIBS[@]}" -o "$TMP/memory-policy"
timeout 20 "$TMP/memory-policy" > "$TMP/memory-policy.log" 2>&1
grep -q 'memory-policy-actual-core-ok' "$TMP/memory-policy.log"
"$CC" "${FLAGS[@]}" "$HERE/anon-accounting.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=mprotect "${LIBS[@]}" -o "$TMP/anon-accounting"
timeout 20 "$TMP/anon-accounting" > "$TMP/anon-accounting.log" 2>&1
grep -q 'anon-accounting-actual-kernel-ok' "$TMP/anon-accounting.log"
"$CC" "${FLAGS[@]}" "$HERE/cow-failure.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=munmap "${LIBS[@]}" -o "$TMP/cow-policy"
for mode in mmap data success ptrace-mmap ptrace-data ptrace-success ptrace-none policy ptrace-policy; do
    ISH_TEST_MEMORY_POLICY=1 timeout 20 "$TMP/cow-policy" "$mode" > "$TMP/cow-$mode.log" 2>&1
    grep -q 'cow-allocation-failure-ok' "$TMP/cow-$mode.log"
done
"$CC" "${FLAGS[@]}" "$HERE/signal-frame-failure.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=do_exit_group "${LIBS[@]}" -o "$TMP/signal-policy"
for mode in mmap data recover policy; do
    ISH_TEST_MEMORY_POLICY=1 timeout 20 "$TMP/signal-policy" "$mode" > "$TMP/signal-$mode.log" 2>&1
    grep -Eq 'signal-frame-(refusal|recovery)-ok' "$TMP/signal-$mode.log"
done
"$CC" "${FLAGS[@]}" "$HERE/memory-upgrade.c" -Wl,--wrap=pthread_rwlock_trywrlock "${LIBS[@]}" -o "$TMP/upgrade-policy"
for mode in precedence {1..11}; do
    for prefix in '' jit-; do
        ISH_TEST_MEMORY_POLICY=1 timeout 20 "$TMP/upgrade-policy" "$prefix$mode" > "$TMP/upgrade-$prefix$mode.log" 2>&1
        grep -q 'memory-upgrade-revalidation-ok' "$TMP/upgrade-$prefix$mode.log"
    done
done
ISH_TEST_MEMORY_POLICY=1 timeout 20 "$TMP/upgrade-policy" jit-contended > "$TMP/upgrade-jit-contended.log" 2>&1
grep -q 'memory-upgrade-revalidation-ok' "$TMP/upgrade-jit-contended.log"
"$CC" "${FLAGS[@]}" "$HERE/cluster-geometry.c" -o "$TMP/cluster-geometry"
"$TMP/cluster-geometry" > "$TMP/cluster-geometry.log"
echo 'memory-policy-gate-ok: protection cap/partial/refund; tickets/concurrency/debt/recovery; CoW9/signal4/upgrade25; geometry'

#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
test "$(uname -m)" = aarch64 || { echo 'native AArch64 oracle required' >&2; exit 1; }
test -x "$ISH_BIN"; test -d "$ROOTFS"
"$CC" -O0 -static -Wall -Wextra "$HERE/fmov-imm.c" "$HERE/fmov-imm.S" -o "$TMP/fmov-imm"
"$CC" -O0 -static -Wall -Wextra "$HERE/syscall-matrix.c" -o "$TMP/syscall-matrix" -lrt
"$CC" -O2 -static -Wall -Wextra -Werror "$HERE/syscall-probes.c" -o "$TMP/syscall-probes" -lrt
check() {
    local log="$1" marker="$2"; shift 2
    timeout -k 5 "$TIMEOUT_S" "$@" > "$log" 2>&1 || { cat "$log"; return 1; }
    if grep -Eq 'SKIP|SAFETY-VALVE|FAIL  |FAILURES: [1-9]' "$log"; then cat "$log"; return 1; fi
    grep -Fq "$marker" "$log" || { cat "$log"; return 1; }
}
check "$TMP/native-fmov.log" 'FAILURES: 0' "$TMP/fmov-imm"
# The UID-change assertion requires privilege; never silently count its skip.
if [ "$(id -u)" = 0 ]; then
    check "$TMP/native-matrix.log" 'PASS: 52    FAIL: 0' "$TMP/syscall-matrix"
else
    check "$TMP/native-matrix.log" 'PASS: 52    FAIL: 0' sudo -n "$TMP/syscall-matrix"
fi
for mode in timer flags signal; do check "$TMP/native-$mode.log" '' "$TMP/syscall-probes" "$mode"; done
tar -C "$TMP" -cf - fmov-imm syscall-matrix syscall-probes |
    timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c \
        'mkdir -p /tmp/upstream-regress && tar -xf - -C /tmp/upstream-regress'
check "$TMP/guest-fmov.log" 'FAILURES: 0' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/fmov-imm
check "$TMP/guest-matrix.log" 'PASS: 52    FAIL: 0' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/syscall-matrix
for mode in timer flags signal; do check "$TMP/guest-$mode.log" '' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/syscall-probes "$mode"; done
echo 'upstream-correctness-gate-ok: FMOV 512 values; syscall matrix 52; timer/flags/signal probes'

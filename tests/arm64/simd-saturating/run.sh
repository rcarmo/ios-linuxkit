#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT/alpine-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
TMP="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$TMP"
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$TMP"' EXIT; fi
test "$(uname -m)" = aarch64
test -x "$ISH_BIN"; test -d "$ROOTFS"
"$CC" -O2 -static -Wall -Wextra -Werror "$HERE/scalar.c" -o "$TMP/scalar"
timeout -k 5 "$TIMEOUT_S" "$TMP/scalar" > "$TMP/native.log" 2>&1
grep -Fxq 'scalar-saturation-ok cases=19696 failures=0' "$TMP/native.log"
tar -C "$TMP" -cf - scalar | timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -ec \
    'mkdir -p /tmp/scalar-saturation && tar -xf - -C /tmp/scalar-saturation'
timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /tmp/scalar-saturation/scalar > "$TMP/guest.log" 2>&1
diff -u "$TMP/native.log" "$TMP/guest.log"
echo 'scalar-saturation-gate-ok: 19696 cases, all widths/aliases, full result/QC/NZCV'

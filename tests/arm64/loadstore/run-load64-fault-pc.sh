#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT_DIR/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT_DIR/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
HOST_TMP="$(mktemp -d)"
trap 'rm -rf "$HOST_TMP"' EXIT

test "$(uname -m)" = aarch64 || { echo 'native AArch64 oracle required' >&2; exit 1; }
test -x "$ISH_BIN"
test -d "$ROOTFS"
"$CC" -O2 -static -Wall -Wextra -Werror "$SCRIPT_DIR/load64-fault-pc.c" -o "$HOST_TMP/load64-fault-pc"
timeout "$TIMEOUT_S" "$HOST_TMP/load64-fault-pc" | grep -qx 'load64-fault-pc-ok cases=18'
tar -C "$HOST_TMP" -cf - load64-fault-pc |
    timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c \
        'mkdir -p /tmp/arm64-load64-fault-pc && tar -xf - -C /tmp/arm64-load64-fault-pc'
timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /tmp/arm64-load64-fault-pc/load64-fault-pc |
    grep -qx 'load64-fault-pc-ok cases=18'
echo 'load64-fault-pc-gate-ok'

#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT_DIR/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT_DIR/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-40}"
HOST_TMP="$(mktemp -d)"
trap 'rm -rf "$HOST_TMP"' EXIT
test "$(uname -m)" = aarch64
"$CC" -O2 -static -Wall -Wextra -Werror "$SCRIPT_DIR/poke-stress.c" -o "$HOST_TMP/poke-stress"
timeout -k 5 "$TIMEOUT_S" "$HOST_TMP/poke-stress" | grep -qx poke-stress-ok
tar -C "$HOST_TMP" -cf - poke-stress |
    timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/tar -xf - -C /tmp
for run in 1 2 3 4 5; do
    timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /tmp/poke-stress |
        grep -qx poke-stress-ok
done
echo poke-stress-gate-ok

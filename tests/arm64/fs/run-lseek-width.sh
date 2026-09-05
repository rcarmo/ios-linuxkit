#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT_DIR/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT_DIR/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
HOST_TMP="$(mktemp -d)"
trap 'rm -rf "$HOST_TMP"' EXIT
test "$(uname -m)" = aarch64
"$CC" -O2 -static -Wall -Wextra -Werror "$SCRIPT_DIR/lseek-width.c" -o "$HOST_TMP/lseek-width"
timeout -k 5 "$TIMEOUT_S" "$HOST_TMP/lseek-width" | grep -qx lseek-width-ok
cp "$SCRIPT_DIR/lseek-python.py" "$HOST_TMP/"
tar -C "$HOST_TMP" -cf - lseek-width lseek-python.py |
    timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/tar -xf - -C /tmp
timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /tmp/lseek-width | grep -qx lseek-width-ok
# Python is intentionally executed through the guest CPU and syscall layer.
timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /usr/bin/python3 /tmp/lseek-python.py |
    grep -qx lseek-python-ok
echo lseek-width-gate-ok

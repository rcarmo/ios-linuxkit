#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PROJECT=$(cd "$HERE/../../.." && pwd)
ISH_BIN=${ISH_BIN:-$PROJECT/build-arm64-linux/ish}
ROOTFS=${ROOTFS:-$PROJECT/alpine-arm64-fakefs}
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$TMP"' EXIT; fi
"${CC:-clang}" -O2 -static -Wall -Wextra -Werror "$HERE/poll-regular.c" -o "$TMP/poll-regular"
timeout -k 3 20 "$TMP/poll-regular" > "$TMP/native.log" 2>&1
grep -q '^poll-regular-ok:' "$TMP/native.log"
tar -C "$TMP" -cf - poll-regular | timeout -k 3 20 "$ISH_BIN" -f "$ROOTFS" /bin/tar -xf - -C /tmp
timeout -k 3 20 "$ISH_BIN" -f "$ROOTFS" /tmp/poll-regular > "$TMP/guest.log" 2>&1
cmp "$TMP/native.log" "$TMP/guest.log"
# On Alpine this exercises BusyBox ash's actual ppoll-before-read path.
timeout -k 3 10 "$ISH_BIN" -f "$ROOTFS" /bin/sh -ec 'echo 123 >/tmp/read-probe; read -r n </tmp/read-probe; test "$n" = 123; rm /tmp/read-probe; echo READ_OK' > "$TMP/shell-read.log" 2>&1
grep -qx READ_OK "$TMP/shell-read.log"
echo poll-regular-gate-ok

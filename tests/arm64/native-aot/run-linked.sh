#!/usr/bin/env bash
# Linked image acceptance/execution gate. Must use a no-emission build.
set -euo pipefail
: "${ISH_BIN:?set ISH_BIN to jit_emit=false CLI}"
: "${ROOTFS:?set ROOTFS to staged Alpine fakefs}"
: "${AOT_RECORD_DIR:?set AOT_RECORD_DIR to completed targeted recorder directory}"
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
BUILD=$(dirname "$ISH_BIN")
grep -q -- '-DISH_JIT_NO_EMIT=1' "$BUILD/compile_commands.json"
test -s "$AOT_RECORD_DIR/manifest.json"
workload=$(cat "$AOT_RECORD_DIR/workload.sh")
for mode in aot off; do
  enabled=1; [[ $mode == off ]] && enabled=0
  timeout -k 3 "${TIMEOUT_S:-120}" env ISH_JIT="$enabled" ISH_JIT_STATS=1 ISH_AOT_FAMILY=0 \
    "$ISH_BIN" -f "$ROOTFS" /bin/sh -ec "$workload; cat /proc/ish/jit" >"$TMP/$mode.log" 2>&1
  grep -qx AOT_TRAIN_OK "$TMP/$mode.log"
done
grep -q 'mode: AOT images only (no executable memory)' "$TMP/aot.log"
grep -q 'images: 4 in use, 0 rejected' "$TMP/aot.log"
grep -q 'segments 0, units 0, code 0 KB' "$TMP/aot.log"
grep -q 'mode: off (gadgets only)' "$TMP/off.log"
grep -q 'AOT installs: 0 ' "$TMP/off.log"
for module in ld-musl busybox libpython libz.so; do
  grep -E "$module.*[1-9][0-9]* */ *[1-9][0-9]* */" "$TMP/aot.log" >/dev/null
done
# Complement the no-emit compiler define and zero-emission counters with an
# object import check. This is not a process-wide mmap audit: the host loader
# still maps the executable and shared libraries.
if nm "$BUILD/libish_emu.a.p/asbestos_guest-arm64_jit.c.o" | grep -Eq ' (memfd_create|mprotect|map_dual|map_jit)$'; then
  echo 'unexpected runtime emitter symbol in AOT-only backend' >&2; exit 1
fi
printf 'linked-aot-gate-ok: four modules used; emission disabled; runtime-off parity\n'

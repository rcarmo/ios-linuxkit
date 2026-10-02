#!/usr/bin/env bash
# Linux adapter executes portable source, TCP backpressure and real signal code.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT=$(cd "$HERE/../../.." && pwd)
: "${BUILD_DIR:?set BUILD_DIR to a matching candidate build}"
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
DEFS=(-D_GNU_SOURCE -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -DISH_NATIVE_OFFLOAD_TEST_HANDLERS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
LIBS=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
WRAPS=()
for name in send recv do_exit fd_close native_fs_context_destroy getsockopt getpeername clock_gettime nanosleep; do WRAPS+=(-Wl,--wrap=$name); done
"${CC:-clang}" -O2 -Wall -Wextra -Werror -Wno-unused-function "${DEFS[@]}" \
    -I"$PROJECT" -I"$BUILD_DIR" -pthread "$HERE/offload-io.c" "$PROJECT/kernel/native_offload.c" \
    "${WRAPS[@]}" "${LIBS[@]}" -o "$TMP/offload-io"
for mode in success partial eintr recv-eintr recv-error zero error eof broken foreign \
    backpressure timeout-read timeout-write cancel-read cancel-write budget-in budget-out \
    deadline return-expired retry-expired full-budget-out cleanup closed cloexec unsupported unix unconnected nested \
    file pipe mismatched wrong-type \
    admit-option admit-peer admit-clock pending pending-kill pending-blocked ignored; do
    timeout -k 3 15 "$TMP/offload-io" "$mode" > "$TMP/$mode.log" 2>&1 || { cat "$TMP/$mode.log"; exit 1; }
    grep -q '^offload-io-ok ' "$TMP/$mode.log" || { cat "$TMP/$mode.log"; exit 1; }
    cat "$TMP/$mode.log"
done
"${CC:-clang}" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/offload-cancel.c" -Wl,--wrap=pthread_mutex_lock "${LIBS[@]}" -o "$TMP/offload-cancel"
timeout -k 3 30 "$TMP/offload-cancel" > "$TMP/cancel.log" 2>&1 || { cat "$TMP/cancel.log"; exit 1; }
grep -q '^native-cancel-lifecycle-ok races=1000 ' "$TMP/cancel.log" || { cat "$TMP/cancel.log"; exit 1; }
cat "$TMP/cancel.log"
printf 'offload-io-gate-ok: 39 dispatcher modes; TCP-only bounded streams; actual signal lifecycle/races/fork isolation\n'

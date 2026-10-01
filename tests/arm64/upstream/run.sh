#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
TMP="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$TMP"
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$TMP"' EXIT; fi
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
"$CC" -O2 -static -pthread "$HERE/lifecycle.c" -o "$TMP/lifecycle"
"$CC" -O2 -static -pthread "$HERE/subms.c" -o "$TMP/subms"
"$CC" -O2 -static "$HERE/open-unlink.c" -o "$TMP/open-unlink"
"$CC" -O2 -Wall -Wextra -Werror -I"$PROJECT" "$HERE/offload-policy.c" -o "$TMP/offload-policy"
check "$TMP/offload-policy.log" 'ALL PASSED' "$TMP/offload-policy"
# Host-linked injection uses the actual candidate archives, not mock kernels.
BUILD_DIR=$(dirname "$ISH_BIN")
HOST_DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- "-DISH_JIT=1" "$BUILD_DIR/compile_commands.json"; then HOST_DEFS+=(-DISH_JIT=1); fi
libs=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
"$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/task-start.c" -Wl,--wrap=pthread_create "${libs[@]}" -o "$TMP/task-start"
"$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/jit-oom.c" -Wl,--wrap=calloc -Wl,--wrap=malloc -Wl,--wrap=do_exit_group "${libs[@]}" -o "$TMP/jit-oom"
"$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" \
    -ffunction-sections -fdata-sections "$HERE/gen-oom.c" "${libs[@]}" -pthread -Wl,--gc-sections -o "$TMP/gen-oom"
lifecycle_marker() {
    case "$1" in
        sleep) echo sleep-deadline-signal-ok;;
        proc) echo proc-exit-bounded-ok;;
        orphan) echo orphan-last-close-ok;;
    esac
}
"$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/anon-accounting.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=mprotect \
    "${libs[@]}" -o "$TMP/anon-accounting"
check "$TMP/anon-accounting.log" 'anon-accounting-actual-kernel-ok' "$TMP/anon-accounting"
"$CC" -O2 -Wall -Wextra -Werror "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/cow-failure.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=munmap \
    "${libs[@]}" -o "$TMP/cow-failure"
for mode in mmap data success ptrace-mmap ptrace-data ptrace-success ptrace-none; do
    check "$TMP/cow-$mode.log" 'cow-allocation-failure-ok' "$TMP/cow-failure" "$mode"
done
"$CC" -O2 -Wall -Wextra -Werror "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/signal-frame-failure.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 -Wl,--wrap=do_exit_group \
    "${libs[@]}" -o "$TMP/signal-frame-failure"
for mode in mmap data recover; do
    marker=signal-frame-refusal-ok
    if [[ "$mode" == recover ]]; then marker=signal-frame-recovery-ok; fi
    check "$TMP/signal-$mode.log" "$marker" "$TMP/signal-frame-failure" "$mode"
done
"$CC" -O2 -Wall -Wextra -Werror "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/memory-upgrade.c" -Wl,--wrap=pthread_rwlock_trywrlock \
    "${libs[@]}" -o "$TMP/memory-upgrade"
for mode in precedence {1..11}; do
    for prefix in '' jit-; do
        check "$TMP/upgrade-$prefix$mode.log" 'memory-upgrade-revalidation-ok' "$TMP/memory-upgrade" "$prefix$mode"
    done
done
check "$TMP/upgrade-jit-contended.log" 'memory-upgrade-revalidation-ok' "$TMP/memory-upgrade" jit-contended
"$CC" -O2 -Wall -Wextra -Werror "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/cow-store.c" -Wl,--wrap=pthread_rwlock_trywrlock \
    "${libs[@]}" -o "$TMP/cow-store"
check "$TMP/cow-store.log" 'cow-store-contention-ok' "$TMP/cow-store"
"$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/offload-exec.c" -Wl,--wrap=native_offload_lookup_exec -Wl,--wrap=native_offload_exec \
    "${libs[@]}" -o "$TMP/offload-exec"
mkdir -p "$TMP/offload-root/bin" "$TMP/offload-root/usr/bin" "$TMP/offload-root/tmp"
printf '#!/missing-interpreter\n' > "$TMP/offload-root/bin/ffmpeg"
chmod 755 "$TMP/offload-root/bin/ffmpeg"
check "$TMP/offload-exec.log" 'offload-exec-shebang-ok' "$TMP/offload-exec" "$TMP/offload-root"
"$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/exit-current.c" -Wl,--wrap=pthread_exit "${libs[@]}" -o "$TMP/exit-current"
check "$TMP/exit-current.log" 'exit-current-ok' "$TMP/exit-current"
check "$TMP/task-start.log" 'task-start-rollback-ok' "$TMP/task-start"
check "$TMP/jit-oom.log" 'jit-oom-guest-kill-dispatch-ok' "$TMP/jit-oom"
check "$TMP/gen-oom.log" 'gen-oom-actual-emitter-ok' "$TMP/gen-oom"
# The renamed/discarded CLI main loses C's implicit return-0 rule. Suppress
# that warning only; exercise the real handler with an exact byte-footprint test.
"$CC" -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-return-type \
    "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread -ffunction-sections -fdata-sections \
    "$HERE/jit-crash-context.c" "${libs[@]}" -Wl,--gc-sections -o "$TMP/jit-crash-context"
if [[ " ${HOST_DEFS[*]} " == *-DISH_JIT=1* ]]; then
    "$CC" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
        "$HERE/native-links.c" "${libs[@]}" -o "$TMP/native-links"
    check "$TMP/native-links.log" "native-refused-link-invalidation-ok" "$TMP/native-links"
fi
check "$TMP/jit-crash-context.log" 'jit-crash-context-ok cases=8' "$TMP/jit-crash-context"
check "$TMP/native-subms.log" ', 0 failed' "$TMP/subms"
check "$TMP/native-open-unlink.log" '8/8 workers clean' "$TMP/open-unlink"
for mode in sleep proc orphan; do
    check "$TMP/native-lifecycle-$mode.log" "$(lifecycle_marker "$mode")" "$TMP/lifecycle" "$mode"
done
check "$TMP/native-fmov.log" 'FAILURES: 0' "$TMP/fmov-imm"
# The UID-change assertion requires privilege; never silently count its skip.
if [ "$(id -u)" = 0 ]; then
    check "$TMP/native-matrix.log" 'PASS: 52    FAIL: 0' "$TMP/syscall-matrix"
else
    check "$TMP/native-matrix.log" 'PASS: 52    FAIL: 0' sudo -n "$TMP/syscall-matrix"
fi
for mode in timer flags signal; do check "$TMP/native-$mode.log" '' "$TMP/syscall-probes" "$mode"; done
tar -C "$TMP" -cf - fmov-imm syscall-matrix syscall-probes lifecycle subms open-unlink |
    timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c \
        'mkdir -p /tmp/upstream-regress && tar -xf - -C /tmp/upstream-regress'
check "$TMP/guest-fmov.log" 'FAILURES: 0' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/fmov-imm
check "$TMP/guest-matrix.log" 'PASS: 52    FAIL: 0' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/syscall-matrix
for mode in timer flags signal; do check "$TMP/guest-$mode.log" '' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/syscall-probes "$mode"; done
check "$TMP/guest-open-unlink.log" '8/8 workers clean' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/open-unlink
check "$TMP/guest-subms.log" ', 0 failed' "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/subms
for mode in sleep proc orphan; do
    check "$TMP/guest-lifecycle-$mode.log" "$(lifecycle_marker "$mode")" "$ISH_BIN" -f "$ROOTFS" /tmp/upstream-regress/lifecycle "$mode"
done
echo 'upstream-correctness-gate-ok: FMOV512; syscall52; timer/flags/signal; sleep/proc/orphan; subms; JIT OOM/crash context; failed-start rollback'

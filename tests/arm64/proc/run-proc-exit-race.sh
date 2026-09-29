#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
ISH_BIN="${ISH_BIN:-$PROJECT/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-90}"
RUNS="${PROC_RACE_RUNS:-2}"
TMP="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$TMP"
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$TMP"' EXIT; fi
[[ "$RUNS" =~ ^[1-9][0-9]*$ ]] && (( RUNS <= 10 ))
test -x "$ISH_BIN"; test -d "$ROOTFS"
BUILD_DIR=$(dirname "$ISH_BIN")
HOST_DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- "-DISH_JIT=1" "$BUILD_DIR/compile_commands.json"; then HOST_DEFS+=(-DISH_JIT=1); fi
libs=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
for fixture in proc-pid-lookup proc-open-locks proc-stat-locks; do
    wraps=()
    if [ "$fixture" = proc-pid-lookup ]; then wraps=(-Wl,--wrap=pid_get_task); fi
    if [ "$fixture" = proc-stat-locks ]; then wraps=(-Wl,--wrap=pthread_mutex_lock); fi
    "${CC:-clang}" -O2 "${HOST_DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
        "$HERE/$fixture.c" "${wraps[@]}" "${libs[@]}" -o "$TMP/$fixture"
    timeout -k 3 30 "$TMP/$fixture" > "$TMP/$fixture.log" 2>&1 || { cat "$TMP/$fixture.log"; exit 1; }
    grep -Fq "$fixture-ok" "$TMP/$fixture.log" || { cat "$TMP/$fixture.log"; exit 1; }
    cat "$TMP/$fixture.log"
done
# Freeze the full stress denominator; custom shorter runs use the guest script
# directly and cannot silently masquerade as this gate.
unset PROC_RACE_SECONDS PROC_RACE_FORKERS PROC_RACE_READERS PROC_RACE_PS
check() {
    local log=$1; shift
    timeout -k 5 "$TIMEOUT_S" "$@" > "$log" 2>&1 || {
        local status=$?
        echo "proc-race: command failed status=$status log=$log" >&2
        cat "$log"; return 1
    }
    if grep -Eq 'SAFETY-VALVE|Assertion|Segmentation fault|proc-race: .*fail|not found' "$log"; then
        cat "$log"; return 1
    fi
    grep -Eq '^PROC_RACE_OK seconds=25 forkers=16 readers=6 execs=[1-9][0-9]* reads=[1-9][0-9]* ps=[1-9][0-9]* backend=(plain|busybox)$' "$log" || { cat "$log"; return 1; }
    cat "$log"
}
check "$TMP/native.log" /bin/sh "$HERE/proc-exit-race.sh"
# Verify absent/broken ps is an error, not a skipped workload reported as pass.
if PROC_RACE_PS=/does-not-exist timeout -k 2 5 /bin/sh "$HERE/proc-exit-race.sh" > "$TMP/negative-ps.log" 2>&1; then
    echo 'proc-race: broken ps unexpectedly passed' >&2; exit 1
else
    status=$?
    [ "$status" = 2 ] || { cat "$TMP/negative-ps.log"; exit 1; }
fi
# Mutation oracle: the old dash idiom cannot collect worker PIDs. A short
# isolated probe reproduces the empty list, but cleans up using the saved PID.
dash -ec 'sleep 10 & p=$!; trap "kill $p 2>/dev/null || :; wait $p 2>/dev/null || :" 0; test -z "$(jobs -p)"; echo dash-job-substitution-empty' > "$TMP/dash-jobs.log"
grep -Fxq dash-job-substitution-empty "$TMP/dash-jobs.log"
# Transfer through fakefs, never write into its backing directory directly.
tar -C "$HERE" -cf - proc-exit-race.sh | timeout -k 5 "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c \
    'mkdir -p /tmp/proc-race-gate && tar -xf - -C /tmp/proc-race-gate'
for ((i=1; i<=RUNS; i++)); do
    check "$TMP/guest-$i.log" "$ISH_BIN" -f "$ROOTFS" /bin/sh /tmp/proc-race-gate/proc-exit-race.sh
done
echo "proc-exit-race-gate-ok: native plus $RUNS guest runs, 25s, 16 forkers + 6 proc/ps readers each"

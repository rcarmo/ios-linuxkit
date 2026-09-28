#!/bin/sh
# Adapted from OpenMinis 80e444f1's proc/exit stress. Debian dash discards the
# job table in $(jobs -p), so upstream's `kill $(jobs -p); wait` never stopped
# its workers. Keep $! in the parent, verify tools, and require worker progress.
# Run under an external watchdog; a hung kernel must be a failure, not a pass.
set -eu
seconds=${PROC_RACE_SECONDS:-25}
forkers=${PROC_RACE_FORKERS:-16}
readers=${PROC_RACE_READERS:-6}
for n in "$seconds" "$forkers" "$readers"; do
    case "$n" in ''|*[!0-9]*|0) echo 'proc-race: positive integer parameters required' >&2; exit 2;; esac
done
[ "$seconds" -le 120 ] && [ "$forkers" -le 32 ] && [ "$readers" -le 16 ] || exit 2
command -v cat >/dev/null
command -v mktemp >/dev/null
[ -x /bin/true ]
# Do not silently drop the ps workload on a minimal fakefs.
ps_mode=plain
if [ -n "${PROC_RACE_PS:-}" ]; then
    ps_bin=$PROC_RACE_PS
elif command -v ps >/dev/null 2>&1; then
    ps_bin=$(command -v ps)
elif command -v busybox >/dev/null 2>&1; then
    ps_bin=$(command -v busybox)
    ps_mode=busybox
else
    echo 'proc-race: ps (or BusyBox ps) is required' >&2
    exit 2
fi
proc_ps() {
    if [ "$ps_mode" = busybox ]; then "$ps_bin" ps -o pid,args; else "$ps_bin" -o pid,args; fi
}
proc_ps >/dev/null || { echo 'proc-race: ps preflight failed' >&2; exit 2; }
cat /proc/$$/cmdline /proc/$$/stat /proc/$$/statm /proc/$$/maps >/dev/null
work=$(mktemp -d /tmp/proc-exit-race.XXXXXX)
pids=''
cleanup() {
    trap - 0 INT TERM
    # On interruption/failure, stop all workers we actually started. Never
    # inspect jobs from a command substitution (its job table can be empty).
    for pid in $pids; do kill -TERM "$pid" 2>/dev/null || :; done
    for pid in $pids; do wait "$pid" 2>/dev/null || :; done
    rm -rf "$work"
}
trap cleanup 0
trap 'exit 130' INT
trap 'exit 143' TERM
fork_worker() {
    # Drop the inherited parent cleanup trap; each worker owns only its stats.
    trap - 0
    id=$1
    count=0
    trap 'printf "%s\n" "$count" > "$work/fork.$id"' 0
    trap 'exit 0' TERM INT
    while :; do
        /bin/true || exit 3
        count=$((count + 1))
    done
}
read_worker() {
    trap - 0
    id=$1
    reads=0
    scans=0
    trap 'printf "%s %s\n" "$reads" "$scans" > "$work/read.$id"' 0
    trap 'exit 0' TERM INT
    while :; do
        for p in /proc/[0-9]*; do
            # ESRCH/ENOENT while a process exits is expected. Successful
            # reads are counted so an entirely broken reader cannot pass.
            if cat "$p/cmdline" "$p/stat" "$p/statm" "$p/maps" >/dev/null 2>&1; then
                reads=$((reads + 1))
            fi
        done
        proc_ps >/dev/null || exit 4
        scans=$((scans + 1))
    done
}
i=0
while [ "$i" -lt "$forkers" ]; do
    fork_worker "$i" & pids="$pids $!"
    i=$((i + 1))
done
i=0
while [ "$i" -lt "$readers" ]; do
    read_worker "$i" & pids="$pids $!"
    i=$((i + 1))
done
sleep "$seconds"
failed=0
# Check for early worker exit before sending TERM. wait also verifies that
# every worker ran its graceful signal trap rather than crashing or dying.
for pid in $pids; do kill -0 "$pid" 2>/dev/null || failed=1; done
for pid in $pids; do kill -TERM "$pid" 2>/dev/null || failed=1; done
for pid in $pids; do wait "$pid" || failed=1; done
pids=''
[ "$failed" = 0 ] || { echo 'proc-race: worker exit failure' >&2; exit 1; }
execs=0
reads=0
scans=0
i=0
while [ "$i" -lt "$forkers" ]; do
    read -r n < "$work/fork.$i"
    [ "$n" -gt 0 ] || { echo "proc-race: forker $i made no progress" >&2; exit 1; }
    execs=$((execs + n))
    i=$((i + 1))
done
i=0
while [ "$i" -lt "$readers" ]; do
    read -r n s < "$work/read.$i"
    [ "$n" -gt 0 ] && [ "$s" -gt 0 ] || { echo "proc-race: reader $i made no progress (reads=$n ps=$s)" >&2; exit 1; }
    reads=$((reads + n))
    scans=$((scans + s))
    i=$((i + 1))
done
printf 'PROC_RACE_OK seconds=%s forkers=%s readers=%s execs=%s reads=%s ps=%s backend=%s\n' \
    "$seconds" "$forkers" "$readers" "$execs" "$reads" "$scans" "$ps_mode"

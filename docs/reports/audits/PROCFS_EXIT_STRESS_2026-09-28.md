# Procfs/exit stress investigation — 28 September 2026

Baseline: ios-linuxkit `v2.2.0` (`15ea6d7f`). Evidence:
`/workspace/tmp/ish-procfs-fix-u3wZ0H/` (pointer
`/workspace/tmp/ish-procfs-fix-path`). This closes the previously unexplained
long-stress timeout with a diagnosed harness defect and a separately captured
kernel lock cycle; it is not a proof that all process-lifetime races are fixed.

## Host

Host-native Orange Pi 6 Plus, CIX P1 (8 Cortex-A720 + 4 Cortex-A520), 12 cores,
16 GB-class RAM/about 14 GiB visible, NVMe; Debian Trixie AArch64,
Linux 6.6.89-cix, Clang 19.1.7, Meson/Ninja. Guest: Debian ARM64 fakefs.
Native stress uses procps; guest uses the existing BusyBox ps applet. No Darwin,
Xcode, device or ASan runtime test was available.

## 1. The old shell harness never stopped on dash

The OpenMinis `80e444f1` fixture creates 16 `/bin/true` fork/exec loops and six
procfs/ps readers, sleeps 25 seconds, then runs:

```sh
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
```

Debian dash has an empty job table inside that command substitution. The same
minimal probe reproduced this natively and under iSH: direct `jobs -p` printed
the child; `$(jobs -p)` printed nothing. The full `sh -x` trace showed argumentless
`kill` followed by `wait` while children continued working. A 40-second GDB
capture showed active exec/exit/proc scans, not a deadlock in that run.

Recording each `$!` in the parent and killing that list made the old script
finish on both released binaries in 25–26 seconds. That alone was insufficient:
`ps` was absent from the minimal fakefs and its status 127 was silently discarded.
Guest apt could not start its HTTP method; no package-install success is claimed.
The installed `/usr/bin/busybox ps -o pid,args` works and is explicitly checked.

The new `tests/arm64/proc/proc-exit-race.sh` checks tools and proc files first,
tracks PIDs, requires all workers to survive until shutdown and exit cleanly,
and requires every forker and every reader to report positive work counts.
Missing/broken ps is a hard failure. An external timeout is mandatory in the
runner. Expected disappearing-PID read errors are tolerated, not ps errors or
an entirely idle worker. Parameters default to the original 25s/16/6 workload;
the gate fixes that denominator rather than silently shortening it.

## 2. Numeric lookup enumerated the PID table

With real ps and per-reader progress checks, released debug runs repeatedly
failed with e.g. `reads=20 ps=0`: a reader had not completed a single ps scan in
25 seconds. Stacks placed BusyBox in proc root enumeration from path lookup.
Every `/proc/<pid>` path component was resolved by enumerating root until the
PID's name matched, examining all lower empty slots under the global PID lock.
Fork churn pushes PIDs upward even when the live population remains small.

Fix: `proc_root_lookup()` resolves the fixed root names or validates a canonical
bounded decimal PID and performs one protected `pid_get_task()` lookup. Root
**directory iteration is unchanged**. Signs, leading zeros, nonnumeric suffixes
and overflow remain rejected; current live-task/zombie behaviour is preserved.
No lookup cache, stale task pointer or additional reference lifetime is added.

Actual-archive regression `proc-pid-lookup.c` advances PID allocation to 30000,
wraps only `pid_get_task` to count slots examined, and tests valid/missing/invalid
names, exit and root enumeration. Baseline `/30000/stat`: **30,001 slots**, exit
134 at the assertion; candidate: all lookups within the **4-slot bound**, passing
release and debug. This is a structural-work test, not a wall-clock benchmark.

## 3. Real inode → PID → memory → inode deadlock

Repeated strict stress with direct lookup still reproduced an intermittent
hang. `hung-4-stacks.txt` captures the cycle:

1. BusyBox opening `/proc` is in `generic_openat` → proc `fstat` →
   `proc_entry_stat`, holding **inodes_lock** while waiting for **pids_lock**.
2. Another BusyBox reader is in `proc_pid_cmdline_show` → `user_read_task`,
   holding **pids_lock** (and the task general lock), waiting for **mem.lock**.
3. Exiting/unmapping tasks hold **mem.lock** for writing and reach `fd_close` →
   `inode_release`, waiting for **inodes_lock**.

Fix: never call filesystem `fstat` while holding the global inode lock in
`generic_openat`. Call fstat first, then acquire the inode through `inode_get`.
Fakefs already retains its metadata inode during open (the v2.2.0 import), so
this preserves that protection. Other filesystems own their open object through
the fd; filesystem callbacks are no longer nested inside inode bookkeeping.

`proc-open-locks.c` mounts the real procfs operations with an fstat checker and
uses actual generic opens of proc root/stat/cmdline/maps. The callback must be
able to try-lock `inodes_lock`. Baseline: EBUSY/16 and assertion exit 134.
Candidate: release/debug pass. Reintroducing only the old `fs/generic.c` into the
otherwise fixed debug binary reproduced timeout **124** in the third of four
runs; the candidate source was never reverted in the checkout.

## Results and boundaries

After both fixes, the first repetition series passed **six release + four debug**
25-second stress runs. The integrated gate then passed a native run plus **two
guest runs per build**, with every forker/reader contributing:

| Build | Guest execs | Successful proc batches | ps scans |
|---|---|---|---|
| release run 1 | 8198 | 1286 | 49 |
| release run 2 | 7812 | 1268 | 48 |
| debug run 1 | 9145 | 1304 | 54 |
| debug run 2 | 8435 | 1211 | 48 |

Both deterministic regressions, all expanded upstream fixtures and the five
existing preservation gates (seek width, poke, FP conversions, proc-mem seek,
load fault-PC) pass on both builds. The open/unlink and orphan tests remain in
the upstream gate and validate the moved fstat lock boundary's filesystem path.

One intermediate direct-lookup-only run exited **139** with an empty log and no
core (core dumps were disabled at that point). Later baseline/mutation attempts
with core enabled did not reproduce that crash. **It is not attributed to the
captured deadlock or claimed independently fixed.** Candidate repeated stress
has not reproduced it; the raw observation remains a separate residual risk.
Do not turn a lack of recurrence into proof of universal race-freedom.

## Reproduction

```sh
CC=clang make build-arm64-linux-all
for build in build-arm64-linux build-arm64-linux-debug; do
  CC=clang EVIDENCE_DIR="/tmp/proc-check-$build" \
    make RELEASE_BUILD_DIR="$build" test-arm64-proc-exit-race
  CC=clang make RELEASE_BUILD_DIR="$build" test-arm64-upstream \
    test-arm64-lseek-width test-arm64-poke-stress test-arm64-fcvt-vector \
    test-arm64-proc-mem-seek test-arm64-load64-fault-pc
done
```

The proc gate defaults to two guest repetitions, plus native stress and negative
harness tests; `PROC_RACE_RUNS` can increase repetition. It compiles structural
regressions against the selected binary's actual archives. Timeouts, early
worker exits, missing ps, zero-progress readers and wrong/missing result markers
are failures, never skips. The 25-second window counts useful work rather than
requiring an arbitrary throughput threshold.

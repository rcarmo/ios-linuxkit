# ios-linuxkit 2.2.1 source release

Date: 28 September 2026. Version **2.2.1**, Apple build **811**.
Annotated source tag: **`v2.2.1`**. Source release only: no iOS archive,
signing, TestFlight/App Store upload or device-validation claim.

## Engineering change

`c44f0fea` — **procfs: break inode/PID/mm lock cycle and bound PID lookup**.
Release metadata is committed separately, on top of [2.2.0](IOS_LINUXKIT_2.2.0.md).

The [investigation](../audits/PROCFS_EXIT_STRESS_2026-09-28.md) resolves the
previously unexplained procfs stress timeout into distinct findings:

1. The original script ran `kill $(jobs -p)`. Debian dash supplies an empty job
   table in that substitution, so the parent waited indefinitely without killing
   its workers. The corrected test records `$!` explicitly and verifies shutdown.
2. Missing guest `ps` was hidden by redirected errors. The gate checks ps before
   starting, uses the installed BusyBox applet when needed, and requires each
   reader to complete successful proc reads and ps scans.
3. Numeric `/proc/<pid>` lookup enumerated all preceding PID slots. Direct,
   bounded decimal lookup preserves directory iteration and live-task semantics.
   The actual-kernel baseline examined **30,001 slots** for `/30000/stat`; the
   fixed regression requires at most four, including final stat bookkeeping.
4. A real deadlock was captured: inode lock → PID lock → target memory lock →
   inode lock. `generic_openat` now calls filesystem fstat **outside inodes_lock**,
   then acquires the inode reference. Fakefs early inode retention is preserved.

Two deterministic actual-archive regressions fail on v2.2.0 (exit 134) and pass
on both candidate builds: high-PID/invalid-name/exit/enumeration lookup, and a
procfs fstat callback checking that the inode lock is available. Reintroducing
only the old fstat lock region into an otherwise fixed binary reproduced timeout
124. No timeout was converted to a pass by increasing the gate's work budget.

## Fresh release validation

Host: Orange Pi 6 Plus; CIX P1, 8 Cortex-A720 + 4 Cortex-A520 cores; 16 GB-class
RAM/about 14 GiB visible; NVMe. Host-native Debian Trixie AArch64,
Linux 6.6.89-cix, Clang 19.1.7, Meson/Ninja. Debian ARM64 fakefs; native procps
and guest BusyBox ps. No changes to hardware power/frequency policy.

Fresh `build-arm64-release-2.2.1` and `build-arm64-debug-2.2.1` configurations
build successfully, **96 steps each**. Existing compiler warnings remain logged.
Evidence: `/workspace/tmp/ish-2.2.1-release-NO4cIL/`; engineering diagnosis:
`/workspace/tmp/ish-procfs-fix-u3wZ0H/`.

| Gate | Fresh release | Fresh debug |
|---|---|---|
| Procfs actual-archive lookup and lock-order regressions | PASS | PASS |
| Native stress + two guest stress repetitions (25s, 16 forkers, 6 readers) | PASS | PASS |
| Expanded upstream gate (FMOV512/syscall52, lifetimes, OOM/accounting/offload) | PASS | PASS |
| Full-width seek | PASS | PASS |
| CPU poke | PASS | PASS |
| AdvSIMD conversions/FP state | PASS | PASS |
| Proc-mem seek | PASS | PASS |
| Precise load fault/retry PC | PASS | PASS |
| Internal continuation | 14/14 | 14/14 |
| CAS128, CLREX/STXR, exclusive widths, LDPSW | Four exact native-output matches | Four exact native-output matches |

Fresh proc stress work counts (every individual worker also passed its positive
progress check):

| Build/run | execs | Successful proc batches | ps scans |
|---|---:|---:|---:|
| Release 1 | 8005 | 1282 | 48 |
| Release 2 | 8074 | 1273 | 49 |
| Debug 1 | 8225 | 1253 | 48 |
| Debug 2 | 8697 | 1261 | 51 |

Including engineering runs, the final lock/lookup changes have completed **10
release and 12 debug** full-load guest runs, 25 seconds each. These counts exclude
the broken-harness runs, earlier failing candidates and reverted-lock mutation.
They establish repeated bounded progress, not an exhaustive concurrency proof.
`make check-docs` passes for 47 Markdown files; expanded link checks pass for 51.
`git diff --check` and all version/build inheritance assertions pass.

```sh
CC=clang make RELEASE_BUILD_DIR=build-arm64-release-2.2.1 \
  DEBUG_BUILD_DIR=build-arm64-debug-2.2.1 build-arm64-linux-all
for build in build-arm64-release-2.2.1 build-arm64-debug-2.2.1; do
  CC=clang make RELEASE_BUILD_DIR="$build" test-arm64-proc-exit-race \
    test-arm64-upstream test-arm64-lseek-width test-arm64-poke-stress \
    test-arm64-fcvt-vector test-arm64-proc-mem-seek test-arm64-load64-fault-pc
  HOST_CC=clang make RELEASE_BUILD_DIR="$build" \
    ROOTFS_DIR="$PWD/debian-arm64-fakefs" test-arm64-internal-continue-fixtures
done
make check-docs
git diff --check
```

Use `EVIDENCE_DIR` to retain artifacts; `PROC_RACE_RUNS` increases stress
repetition. Missing tools, timeout, early/abnormal worker exit and zero progress
fail the proc gate. Native UID tests in the upstream gate require noninteractive
sudo. All four project build settings advance to 811; both ARM64 schemes inherit
marketing version 2.2.1. Inherited non-ARM64 marketing version is unchanged.

## Residual risks and limits

- One **intermediate direct-lookup-only** stress run crashed with status 139,
  empty output and core dumps disabled. Subsequent baseline/mutation attempts
  with core enabled did not reproduce it. It remains **unattributed**, not a
  separately proven fix. No final candidate run reproduced it; the raw failure
  is retained in the investigation rather than hidden.
- The diagnosed lock-order timeout and corrected full-load test pass, but existing
  task teardown safety-valve behaviour is not certified race-free.
- ASan runtime libraries are still unavailable. No Darwin/iOS/Xcode/device
  validation was performed. Repeat concurrent proc/ps + fork/exec/exit on the
  intended device archive; the lock-order change is shared source.
- Existing broad-suite Debian bootstrap, clone3/alternate-stack coverage and
  native LDXP/STLXP SIGBUS limitations remain. The load-PC fixture retains its
  isolated-unmap scope, not PROT_NONE or second-page-only fault coverage.
- No general performance percentage is claimed. The lookup regression measures
  algorithmic work, and the stress gate measures positive progress, not speed.
  The policy/cache/experimental exclusions from 2.2.0 remain in force.

## Publication

Push the metadata commit to `master`, then create and push annotated `v2.2.1`
on that exact commit; verify remote master and the peeled tag agree. Preserve
all previous tags. This tag records source provenance, not an iOS binary release.

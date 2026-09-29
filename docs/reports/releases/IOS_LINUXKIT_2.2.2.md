# ios-linuxkit 2.2.2 source release

Date: 29 September 2026. Version **2.2.2**, Apple build **812**.
Annotated source tag: **`v2.2.2`**. Source release only: no iOS archive,
signing, TestFlight/App Store upload or device-validation claim.

## Changes since 2.2.1

`1e7ad5e9` — **ARM64 CLI crash-handler field writes and layout checks**.
Release metadata is committed separately, on top of the current gadget engine.

- Store `segfault_was_write` as its actual one-byte `bool`, not a four-byte
  `int`. The previous store also touched three adjacent padding bytes; it is
  not claimed to have corrupted a live field or caused the earlier proc crash.
- Compile-time checks tie five recovery offsets to the generated C/assembly
  layout, including local precise-retry state. A deliberately stale generated
  offset fails compilation in the engineering regression.
- Declare the generated-offset header dependency for consumers of the Meson
  `ish` dependency, preventing a clean-build ordering race.
- Eight real-handler synthetic-context cases check SIGSEGV/SIGBUS, read/write,
  exact byte footprint, precise/fallback PC and saved-SP/trampoline selection.
  The pre-fix code fails the byte-footprint assertion.

The engineering [audit](../audits/JIT_AOT_INVESTIGATION_2026-09-29.md) records the
selective adaptation from OpenMinis and the separate experimental investigation.
**This patch does not import or enable native JIT/AOT, include its experimental
restart repair, or upgrade the packaged Alpine userland.** Those changes are
being developed separately. No performance improvement is claimed for 2.2.2.

## Fresh release validation

Host: Orange Pi 6 Plus; CIX P1, 8 Cortex-A720 + 4 Cortex-A520 cores; 16 GB-class
RAM/about 14 GiB visible; NVMe. Host-native Debian Trixie AArch64,
Linux 6.6.89-cix, Clang 19.1.7, Meson/Ninja. Debian ARM64 fakefs, native procps
and guest BusyBox ps. Hardware power/frequency policy unchanged.

Fresh `build-arm64-release-2.2.2` and `build-arm64-debug-2.2.2` configurations
build successfully, **96 steps each**. Build types verified as release/debug;
existing compiler warnings retained in logs. Evidence and binary hashes:
`/workspace/tmp/ish-2.2.2-release-7SHKlY/`, pointer
`/workspace/tmp/ish-2.2.2-release-path`.

| Gate | Release | Debug |
|---|---|---|
| Expanded upstream correctness/lifetime/OOM/accounting/offload gate | PASS | PASS |
| Real CLI crash-handler context regression | 8/8 | 8/8 |
| Full-width lseek | PASS | PASS |
| CPU poke stress | PASS | PASS |
| AdvSIMD conversions / FP state | PASS | PASS |
| Proc-mem seek | PASS | PASS |
| Precise load fault/retry PC | PASS | PASS |
| Procfs lookup/lock-order regressions | PASS | PASS |
| Native proc stress + two full guest repetitions | PASS | PASS |
| Internal continuation | 14/14 | 14/14 |
| CAS128, CLREX/STXR, exclusive widths, LDPSW | Four exact native-output matches | Four exact native-output matches |

Procfs stress retains **25 seconds, 16 forkers and 6 proc/ps readers**, an
external watchdog and positive progress/shutdown checks for every worker:

| Build/run | execs | Successful proc batches | ps scans |
|---|---:|---:|---:|
| Release 1 | 8053 | 1292 | 49 |
| Release 2 | 8064 | 1278 | 48 |
| Debug 1 | 8353 | 1236 | 48 |
| Debug 2 | 7982 | 1200 | 47 |

These are bounded correctness/progress results, not an exhaustive concurrency
proof. Missing tools, timeout, skip and zero-progress workers are failures.
Guest fixtures were transferred through guest tar, not fakefs backing writes.

```sh
CC=clang make RELEASE_BUILD_DIR=build-arm64-release-2.2.2 \
  DEBUG_BUILD_DIR=build-arm64-debug-2.2.2 build-arm64-linux-all
for build in build-arm64-release-2.2.2 build-arm64-debug-2.2.2; do
  CC=clang make -o build-arm64-linux RELEASE_BUILD_DIR="$build" \
    test-arm64-upstream test-arm64-lseek-width test-arm64-poke-stress \
    test-arm64-fcvt-vector test-arm64-proc-mem-seek test-arm64-load64-fault-pc \
    test-arm64-proc-exit-race
  HOST_CC=clang make -o build-arm64-linux RELEASE_BUILD_DIR="$build" \
    ROOTFS_DIR="$PWD/debian-arm64-fakefs" test-arm64-internal-continue-fixtures
done
make check-docs
git diff --check
```

All four project build values advance to 812. Both ARM64 schemes inherit
marketing version 2.2.2; inherited non-ARM64 marketing version is unchanged.
Metadata, Markdown links and final committed-tree smoke tests are release gates.

## Limits

No Darwin/iOS/Xcode/device or sanitizer validation. The real CLI handler is
exercised with synthetic contexts on Linux, not an Apple signal-return test.
The earlier intermediate proc-stress exit139 remains unattributed; neither this
release nor the separate native/AOT repair is claimed to explain it. Existing
teardown safety-valve, broad Debian bootstrap, clone3 alternate-stack and native
LDXP/STLXP SIGBUS limitations remain. Load-PC coverage remains scoped as in the
[2.2.1 release](IOS_LINUXKIT_2.2.1.md).

## Publication

Push the metadata commit to `master`, then create/push annotated `v2.2.2` on that
exact commit; verify remote master and the peeled tag agree. Preserve all prior
tags. This is source provenance, not delivery of a signed iOS binary.

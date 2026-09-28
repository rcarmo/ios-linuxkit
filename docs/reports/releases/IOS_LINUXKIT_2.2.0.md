# ios-linuxkit 2.2.0 source release

Date: 28 September 2026. Release version **2.2.0**, Apple build **810**.
Source tag: **`v2.2.0`**. This is a source release, not an iOS archive,
TestFlight/App Store upload or physical-device validation record.

## Scope and provenance

Adopts the beneficial, validated September OpenMinis imports on top of
[v2.1.3](IOS_LINUXKIT_2.1.3.md), retaining local ARM64, precise-PC, FP state,
full-width seek, poke, deferred-mm and JIT continuation semantics. The user
requested a minor release for this combined runtime/integration update.

The five engineering commits remain separate from release metadata:

| Commit | Change |
|---|---|
| `1723ba66` | FMOV immediate decoding; POSIX timer delivery and signal consumption; open flags/path bounds, waitid and archive/bind-stat correctness. |
| `1625d4d5` | Interruptible precise sleeps/futex waits; task/mm/proc and inode lifetimes; JIT allocation failure and failed thread-start rollback; Mach-right release and app launch consistency. |
| `9e25075a` | Generic native-offload exact-path, environment opt-out and shebang-wrapper safeguards. |
| `78e96ea1` | Balanced logical anonymous-page accounting across maps, protection changes, fork/CoW, lazy commits, remap and allocation failures. |
| `8a695325` | Debug pointer invariant limited to uninterrupted lock intervals; full upstream disposition and validation record. |

The [import audit](../audits/OPENMINIS_IMPORT_2026-09-28.md) accounts for all
**55 non-merge commits** in the 69-commit upstream range ending at `b08c12af`.
That audit's no-push/no-version-change statements describe the engineering
checkpoint before this separately authorised release.

### Compatibility and exclusions

- Generic ffmpeg/ffprobe offloads now claim only exact `/bin`, `/usr/bin` and
  `/usr/local/bin` paths. Private/relative binaries and readable shebang wrappers
  fall through to normal guest exec. `NO_OFFLOAD=1` or the upstream-compatible
  `MINIS_NO_FFMPEG_OFFLOAD=1` disables generic offloading. Synthetic Apple-only
  commands are unaffected. This selection policy is not a sandbox.
- Every positive nanosleep is honoured; upstream's short-sleep yield policy was
  not adopted. Interruption returns the remaining duration where appropriate.
- The anonymous ledger counts logical committed guest pages, not host RSS.
  Device-derived caps, host-page clustering and footprint/fork governors remain
  excluded; the existing cap value is unchanged.
- Context-insensitive global path caching, stale exec-stat caching, reverted
  poll/epoll candidates, experimental JIT/AOT/network branches, forced Node
  single-generation GC and untested cooperative in-process abort hooks are not
  included. See the audit for each individual disposition.

## Release validation

### Host and build

- Orange Pi 6 Plus, CIX P1 (8 Cortex-A720 + 4 Cortex-A520), 12 cores,
  16 GB-class RAM (about 14 GiB Linux-visible), NVMe storage.
- Host-native Debian Trixie AArch64, Linux `6.6.89-cix`, Clang `19.1.7`;
  Meson/Ninja via the repository Makefile; Debian ARM64 fakefs.
- Fresh, separate `build-arm64-release-2.2.0` and `build-arm64-debug-2.2.0`
  configurations: **96 build steps each**, both successful. These are not
  reused engineering binaries. Existing compiler warnings remain in the logs.
- Evidence: `/workspace/tmp/ish-2.2.0-release-K1W8ah/`; binary SHA-256 values,
  complete build/gate logs, native outputs and fixture artifacts retained.

### Passing gates on both fresh binaries

| Gate | Result |
|---|---|
| Expanded upstream correctness/lifetime | PASS: FMOV **512 values**, syscall matrix **52/52**, timer/flag/signal probes, precise waits and bounded lifecycle fixtures. Native oracles pass too. |
| Allocation and task-start injection | PASS: actual emitter/kernel/JIT archives; **300 failed clones + 100 failed app-task starts** per build; allocation rollback and lock/dispatch checks. |
| Anonymous accounting | PASS: actual-kernel PROT_NONE, partial unmap, replacement, protection/fork/CoW, lazy/high-VA reservation, moved/in-place remap and injected map/data/protection failures. |
| Offload policy/exec | PASS: **39 policy assertions** plus actual sys_execve filesystem/shebang selection with wrapped native entry points. Linux's live offload registry remains stubbed. |
| `test-arm64-lseek-width` | PASS: full-width seek and Python sparse-file integration. |
| `test-arm64-poke-stress` | PASS: acknowledged signal delivery during guest computation. |
| `test-arm64-fcvt-vector` | PASS: AdvSIMD conversions and FP state. |
| `test-arm64-proc-mem-seek` | PASS: local native-seek semantics retained. |
| `test-arm64-load64-fault-pc` | PASS: exact fault/retry PC gate, with its existing isolated-unmap scope. |
| Internal continuation | **14/14** release and **14/14** debug, including default-off, branch families, invalidation, call adjacency and fault-PC checks. |
| CAS128, CLREX/STXR, exclusive widths, LDPSW | All **four** supplemental static fixtures match native output exactly on each build. |

The upstream gate also passes 30 bounded proc/exit rounds, 50 orphan/rename
rounds and concurrent open/unlink with **8/8 workers clean**, native and guest.
Submillisecond tests report **3489 pass / 0 fail** release and **3461 / 0** debug;
counts depend on elapsed time. Shortest 500us futex waits were 535us/529us.
The continuation runner uses explicit `HOST_CC=clang` to compile the same static
fixture on native AArch64; the minimal Debian guest does not require gcc.

`make check-docs` passes for **45 Markdown files**; `git diff --check` passes.
All four Xcode project build settings equal 810; both ARM64 schemes inherit
marketing version 2.2.0. The inherited non-ARM64 marketing version is unchanged;
the project-wide build number advances for all four configurations. The
read-only delegated consistency check timed out, so no independent-review
result is claimed; metadata and evidence were checked directly.

### Performance evidence

The engineering audit recorded ten alternating baseline/candidate pairs on
CPU11: 10,000 open/fstat/close operations per sample on the same fakefs.
Median **0.371973s → 0.320039s**, approximately **14% lower**, with the candidate
faster in **10/10 pairs**. Baseline was saved v2.1.3; this measures combined
tranches, not individual patches. CPU affinity was fixed, frequency was not.
This release pass reruns correctness, not that benchmark. No application-wide,
shell-startup, iOS, thermal or battery improvement is inferred.

## Reproduction

```sh
CC=clang make RELEASE_BUILD_DIR=build-arm64-release-2.2.0 \
  DEBUG_BUILD_DIR=build-arm64-debug-2.2.0 build-arm64-linux-all
for build in build-arm64-release-2.2.0 build-arm64-debug-2.2.0; do
  CC=clang make RELEASE_BUILD_DIR="$build" test-arm64-upstream \
    test-arm64-lseek-width test-arm64-poke-stress test-arm64-fcvt-vector \
    test-arm64-proc-mem-seek test-arm64-load64-fault-pc
  HOST_CC=clang make RELEASE_BUILD_DIR="$build" \
    ROOTFS_DIR="$PWD/debian-arm64-fakefs" test-arm64-internal-continue-fixtures
done
make check-docs
git diff --check
```

Set `EVIDENCE_DIR` for the upstream runner to retain its artifacts. The native
UID-changing syscall matrix requires root or noninteractive sudo.

## Unresolved limits and Apple handoff

- Long upstream procfs shell stress previously timed out with status 124 and
  never printed `PROC_RACE_OK`. It is **not a pass**; bounded proc tests do not
  close it. Existing teardown safety-valve behaviour is not certified race-free.
- Missing Clang 19 AArch64 ASan libraries prevent an ASan link/run. No sanitizer
  pass is claimed.
- Broad Debian package detection, clone3/alternate-stack coverage and native
  LDXP/STLXP SIGBUS remain open as recorded in the import audit. No new broad
  package/runtime-suite pass is claimed by these focused results.
- The load-PC fixture unmaps both isolated pages; it does not establish
  second-page-only or PROT_NONE fault correctness. Existing near-neighbour
  read-fault recovery remains documented in [LIMITATIONS.md](../../LIMITATIONS.md).
- Darwin Mach-right release, app failed-start cleanup and launch environments
  are source-reviewed only. Linux offload tests do not validate Darwin registry,
  host spawning or real in-process FFmpeg cancellation.
- No Xcode build, signing, archive, device smoke test or upload was performed.
  Build both ARM64 schemes on macOS and test the exact archive on a device:
  terminal/upgrade launches, native-offload wrappers and opt-outs, interrupted
  sleeps, repeated fork/exec/exit, pressure behaviour and foreground/background.

## Publication

The release metadata commit follows the five engineering commits. Push `master`
first, then create and push annotated tag `v2.2.0` on that exact commit, verifying
remote master and the peeled tag agree. The tag is source provenance, not proof
of an iOS binary distribution. Previous release tags are preserved.

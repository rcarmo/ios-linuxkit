# ios-linuxkit 2.1.2 source release

> **Dated record — 5 September 2026:** Source release `v2.1.2`. Linux-host validation is not an iOS archive, device test or App Store upload.

## Version and scope

- ARM64 marketing version: `2.1.2` in `app/AppARM64.xcconfig`.
- Apple build number: `808` in all four project build configurations.
- Source tag: annotated `v2.1.2`, targeting the release commit on `master`.
- Inherited non-ARM64 version: unchanged at `1.0.0` in `app/Project.xcconfig`.
- Both shared ARM64 app schemes use the ARM64 version directly or through `AppARM64-ffmpeg.xcconfig`.

This compatible patch follows [2.1.1](IOS_LINUXKIT_2.1.1.md). The release commit changes metadata and documentation only; the two engineering changes retain their separate commits.

## Included changes

### Correct full-width seek returns — `a5d571f2`

ARM64 syscall writeback no longer interprets valid `lseek` offsets in
`0xfffff001..0xffffffff` as zero-extended 32-bit errnos. The underlying seek
already returned the correct 64-bit value; the corruption occurred when it was
written into guest x0. Guest Python sparse-file seeks now return their offsets
instead of raising `PermissionError` or another spurious errno.

The static raw-syscall/libc fixture fails on the saved baseline and passes on
the candidate with identical bytes. Tests retain real Python integration,
SET/CUR/END checks, boundaries around 4 GiB, larger offsets, pipe/invalid-fd
errors and position preservation. See the [correctness investigation](../audits/ARM_LINUX_LSEEK_2026-09-05.md) for commands, hashes and native-reference limitations.

### Avoid unnecessary poke exchanges — `e1417b6e`

A relaxed atomic load checks for a pending CPU poke before the existing acquire
exchange consumes it. This avoids an unconditional read/modify/write at every
interpreter block boundary while leaving pending pokes for consumption and
retaining the existing interrupt cadence. No executable-memory allocation,
native offload or Linux-only execution shortcut is introduced.

Two controlled CPU-pinned ARM Linux Python compute series, 15 pairs each,
measured median wall times of 4.467 → 4.370 s and 4.457 → 4.390 s:
**1.5–2.2% median improvement**, with 23/30 faster pairs. Startup and I/O showed
no demonstrated benefit. These are the engineering measurements, not a new
release benchmark or an iOS speedup claim. See the [profiling and benchmark report](../benchmarks/ARM_LINUX_POKE_2026-09-05.md) for spreads, resource use, warm-cache treatment and the inconclusive preliminary run.

## Release validation

Host: Orange Pi 6 Plus; CIX P1 (CD8180/CD8160), 4 Cortex-A520 + 8 Cortex-A720,
16 GB class RAM (about 14 GiB visible), NVMe/ext4 workspace. Debian Trixie,
AArch64 kernel `6.6.89-cix #15`, Clang 19.1.7, Meson 1.7.0, Ninja 1.12.1.
Host-native process; guest Debian 13.5 fakefs with Python 3.13. Guest fixtures
run through Asbestos and the guest syscall layer, not host-native offload.

Passed on 5 September 2026:

- Fresh out-of-tree Clang release and debug builds, 96 steps each.
- All four focused gates against **both** fresh binaries: full-width seeks
  (including real guest Python), poke stress (five repetitions, 5,120
  acknowledged signals per guest gate), FCVT vector conversions and proc-mem
  seeks. Static fixture runners also execute their native ARM64 oracle.
- Supported default `CC=clang make build-arm64-linux-all`.
- `make check-docs`: 41 Markdown files, all local link targets present.
- Exact metadata checks: ARM64 `2.1.2`, four build fields `808`, inherited
  `1.0.0`, FFmpeg scheme configuration inheritance unchanged.
- `git diff --check` and review of the metadata/documentation-only diff.

Fresh binary SHA-256:

- release: `769d47083ff6ea66e78b41de41ab5818a0b06309526a4874b3dfea46388f0373`
- debug: `f626bca50ff71b3517ef6afe01087672e7e65c5e0aa122be4aa9f515027ee6c4`

Builds retain existing warnings, principally incompatible syscall
function-pointer casts and unused helpers/parameters. This release commit
changes no C, assembly, build flags or test implementations. No performance
measurements were repeated for this metadata/documentation-only commit.

## Known failures and coverage limits

The engineering run on 5 September, not a full-suite release rerun, found:

- The broad Debian runtime gate passes four base rows, then stops before C
  coverage: platform detection retains a blank line and selects Alpine's
  `build-base`. This harness failure remains unresolved.
- The existing glibc alternate-stack thread fixture exits at `pthread_create`:
  guest `clone3` returns `EINVAL`. Native execution passes, but the failing
  guest run does not exercise alternate-stack behaviour. This remains unresolved.
- Nine other static guest fixtures passed on baseline, candidate and debug,
  including signal context, precise faults, IPC, barriers and self-modifying
  code. They supplement rather than replace the blocked broad suite.

No full language/runtime matrix, sanitizer or fuzzing result is claimed for
this release. Historical reports remain scoped to their named revisions.

## iOS validation still required

No iOS cross-build, Xcode archive, signing, installation, physical-device run or
App Store upload was performed. Before device distribution:

1. Build and sign both intended ARM64 schemes on macOS; verify version/build.
2. Run guest Python sparse-file seeks and invalid-fd/pipe error cases against
   sandbox-backed storage, confirming cleanup and storage-limit behaviour.
3. Run poke stress and sustained guest computation interrupted by signals;
   check responsiveness and signal-return state.
4. Exercise thread creation, foreground/background transitions and memory
   pressure; investigate the known `clone3` blocker for the packaged libc.
5. Measure thermals, battery use and interactive behaviour on the device;
   do not extrapolate the Linux compute improvement.

The inherited Fastlane upload lane still targets upstream iSH and is not a
validated distribution path for this fork.

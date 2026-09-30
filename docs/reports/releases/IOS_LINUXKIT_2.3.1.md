# ios-linuxkit 2.3.1 — 30 September 2026

Source version **2.3.1**, Apple build **814**, adds reusable AOT tooling and
operating guides. The emulator backend, code version10/ABI `3f650e41`, Alpine
3.24.2 pin and default gadget execution are unchanged from 2.3.0. Existing iOS
schemes keep AOT disabled. No Apple archive was signed or uploaded for this
release.

## Included work

The three prototype commits `5c855b1a`, `f78036a8` and `83c2931f` are included
through the fast-forward of master from `7f2ce5d8` to `83c2931f`:

- versioned seed/generated/build manifests, hashes, staging and explicit modes;
- fakefs snapshot/export/restore, retained recordings and no-retraining rebuild;
- binary-bound Apple ABI contracts, defined-symbol checks and portable module
  extraction using host tar;
- logical guest identity, image-PC, timing and peak-RSS test tools.

Two release fixes accompany the documentation review: recorder inventory uses
APK3's installed-version query, and refusing an existing publication destination
preserves validated build directories and pending metadata. Both have regressions.

The [Linux AOT guide](../../NATIVE_AOT_BUILD_PLAN.md),
[artifact guide](../../NATIVE_AOT_ARTIFACT_KIT.md) and
[iOS guide](../../NATIVE_AOT_IOS.md) give commands, prerequisites, failure handling
and test requirements. The [documentation review](../audits/DOCUMENTATION_REVIEW_2026-09-30.md)
records all 13 maintained documents, corrected discrepancies, writing rules and
checks. Historical reports and vendor text keep their original observations.

## Release verification

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four
Cortex-A520 cores, 16GB-class RAM (about 14GiB visible), NVMe/ext4, Debian Trixie,
Linux6.6.89-cix. Host-native service; workspace `/workspace` resolves to
`/home/agent/workspace`. Clang19.1.7, Meson1.7.0, Ninja1.12.1, Bun1.4.2.

Fresh directories `build-arm64-2.3.1-{gadget,aot}-{release,debug}` build all four
configurations. Gadget uses `jit=false`; AOT uses `jit=true`, `jit_emit=false`
and the four newly generated images. Each runs on its own restored Alpine
3.24.2 guest. Tests modify those copies only.

| Check | Result |
|---|---|
| Fresh gadget/AOT release/debug builds | Pass, four configurations. |
| Upstream correctness/lifecycle/OOM, poll, FCVT, exact load-PC, proc-mem/lseek/poke | Pass in all four. |
| Internal continuation | 14/14 in each configuration. |
| Full procfs/exit stress | Native control and two guest repetitions per configuration; 25s/16 forkers/6 readers each; progress and shutdown checks pass. |
| No-retraining generation | Four image assemblies byte-identical to retained prototype images. |
| Fresh recording pipeline | All four targeted recordings/images complete with APK3 installed-version inventory and a final manifest. |
| Linked AOT release/debug | Four accepted, zero rejected, per-module installs, zero emission and runtime-off parity. |
| Actual image execution | All four debug-image PCs hit within image text, emitter region NULL, normal exit. |
| Native restart at O0/O2 | 100 actual faults each, exact PC/address/writeback and no duplicate prefix effects. |
| Native instruction oracles at O0/O2 | 8576 integer +2720 memory comparisons each, plus SIMD/family/ownership checks. |
| Kit regressions | 10 tests /41 assertions. |
| Generator/recorder regressions | 3 tests /24 assertions. |
| Documentation | 58 local Markdown target checks and four maintained heading-fragment checks pass; 13 maintained docs pass selected writing rules; scanner tests 2 /6. |
| Shell snippets and version fields | 52 blocks parse; named test/record Make targets exist; all four project build numbers are814 and ARM64 version is2.3.1. |
| Rootfs downloader | Hash/architecture/fetch/pin failure and old-bundle preservation tests pass. |

Evidence: `/workspace/tmp/ish-2.3.1-release-GR6Cdk/`, pointer
`/workspace/tmp/ish-2.3.1-release-path`. Per-configuration build, upstream,
focused, proc and linked logs are retained. `image-execution-final/` contains
complete PC evidence; `native-restart/` contains the real-fault/oracle binaries.
The first image-PC attempt was interrupted by the enclosing shell's 600s budget
and is excluded. A separate full-budget run passed.

## Performance and artifacts

This release does not change translated code or introduce a new performance
measurement. The [29 September prototype](../audits/AOT_ARTIFACT_KIT_2026-09-29.md)
measured shell1.122×, zlib1.130×, Python0.871× and startup0.955× in five paired
host runs. Python peak RSS increased from18.3MiB to54.1MiB. Preserve those costs
when selecting workloads for AOT.

The delivered 2.3.0 prototype Linux and iOS-input archives retain their original
names, source revisions and checksums. They were not rebuilt or relabelled for
2.3.1. Current source includes the corrected guides and publisher; rebuild a new
bundle to include those tools.

## Apple work

Apple target layout/symbol checks, app native fault recovery, isolated Xcode
settings, Mach-O membership/constructors, signing, device lifecycle/stress and
performance/energy checks still require Apple hardware and SDKs. Linux recording
reuse must pass the actual target contract before generation and device checks
before distribution. The source tag identifies this revision; the release
contains no signed accelerated iOS app.

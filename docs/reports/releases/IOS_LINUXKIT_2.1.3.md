# ios-linuxkit 2.1.3 source release

> **Dated record — 5 September 2026:** Source release `v2.1.3`. Linux-host validation is not an iOS archive, device test or App Store upload.

## Version and scope

- ARM64 marketing version: `2.1.3` in `app/AppARM64.xcconfig`.
- Apple build number: `809` in all four project build configurations.
- Source tag: annotated `v2.1.3`, targeting the release commit on `master`.
- Inherited non-ARM64 version: unchanged at `1.0.0` in `app/Project.xcconfig`.
- Both shared ARM64 schemes inherit the same version, directly or through `AppARM64-ffmpeg.xcconfig`.

This compatible performance patch follows [2.1.2](IOS_LINUXKIT_2.1.2.md). The
engineering change is committed separately; the release commit changes only
metadata and documentation. No new runtime capability, rootfs, build flags,
TLB layout or permission handling is introduced.

## Included engineering change — `3a77cb9f`

Full commit: `3a77cb9f738e071ecbc273f8dbd32888468fc505`,
`arm64: inline precise fault PC save in common load gadget`.

Normal-register unsigned-immediate `LDR X` now saves its precise retry PC inside
the load gadget. A paired stream load consumes operands and PC together,
removing one indirect gadget dispatch and one eight-byte stream word per
affected load. The existing LDR+CBZ/CBNZ fusion already saves its own LDR PC;
other memory forms retain their previous saves. TLB tag, generation, miss and
cross-page handling remain unchanged.

All execution still uses precompiled AArch64 gadgets and data-only translated
programs. There is no executable-memory allocation, native offload or Linux-only
execution shortcut. The new static native-oracle gate checks exact LDR signal
PC, pre-load registers and retry state across 18 aligned, unaligned, split-load,
CBZ/CBNZ and zero/nonzero cases.

Two controlled, CPU11-pinned, 15-pair Linux compute batches against 2.1.2
measured **4.39134 → 4.27507 s** and **4.39516 → 4.31354 s**: **2.65% and 1.86%
median reductions**, with **23/30 pairs faster** and **1.575% mean paired gain**.
Python startup and temporary-file workloads showed smaller gains; shell startup
remained noisy and no shell improvement is claimed. These are engineering
measurements, not new release benchmarks or an iOS speedup claim. Do not add
these percentages to the preceding poke pass.

The preliminary paired-load/shifted-add TLB candidate was rejected because its
gain did not repeat. See the [full load-dispatch report](../benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md) for raw-evidence paths, spreads, hashes and exclusions.

## Release validation

Host: Orange Pi 6 Plus; CIX P1 (CD8180/CD8160), 4 Cortex-A520 + 8 Cortex-A720,
16 GB-class RAM (about 14 GiB Linux-visible), NVMe/ext4 workspace. Host-native
Debian Trixie, AArch64 kernel `6.6.89-cix`, Clang 19.1.7, Meson 1.7.0, Ninja
1.12.1. Guest Debian 13.5 fakefs with Python 3.13. No native execution shortcut.

Passed on 5 September 2026:

- Fresh out-of-tree release and debug Clang builds, 96 steps each, via
  `CC=clang make build-arm64-linux-all` with separate build-directory overrides.
- Supported default `CC=clang make build-arm64-linux-all`.
- All **five focused gates on both fresh binaries**: full-width lseek/Python,
  poke stress, FCVT vector conversions, proc-mem seek semantics and load fault-PC
  recovery. Static fixture runners also execute their native AArch64 oracles.
- Four additional native-oracle fixture comparisons on each fresh build:
  CAS128, CLREX/STXR, exclusive load widths and LDPSW pairs — eight guest rows,
  zero exits and identical native stdout.
- The engineering gate also passes unchanged 2.1.2 and detects a deliberate
  wrong-PC mutation (exit 40). That mutation was restored and rebuilt; it is
  not part of either release binary.
- `make check-docs`: 43 Markdown files with all local targets present; expanded
  checks pass 50 files, also covering the root issue/security/licence, tooling
  and asset notes.
- Metadata checks: ARM64 `2.1.3`, four build fields `809`, inherited `1.0.0`,
  unchanged ARM64 FFmpeg configuration inheritance.
- `git diff --check` and review of the metadata/documentation-only release diff.

Fresh binary SHA-256:

```text
release 870033b3fda4298f3eb436b10fb362bcabc337311e1a2da25a1135019cc4c1a6
debug   2beab49c9f8182f0569ecfa081e3423f02c63eb55eba554e96fcc374e9a64bc4
```

Local evidence: `/workspace/tmp/ish-2.1.3-release-91C0KI/`. Builds retain existing
warnings, including syscall function-pointer casts and unused helpers or
parameters. No performance measurement was repeated for the metadata-only
release commit.

## Documentation review

All maintained guides were reviewed: project and documentation indexes,
architecture, Linux development, iOS application, validation, limitations,
contributing and releases. Updates add the load-PC gate, paired timing method,
correctness-test requirements for performance changes, current evidence and
2.1.3 metadata. The architecture text now distinguishes disabled broad synthetic
fault recovery from the **still-enabled nearby-page and targeted V8 recovery**.

Security policy, issue template, inherited Fastlane/tooling/asset notes and
historical report references were checked for release applicability. Older
release, audit, benchmark, legacy and licence records retain their original
versions and provenance; they were not blanket-rewritten to 2.1.3.

## Known failures and exclusions

- Broad Debian runtime coverage was not rerun. Previously, `detect_platform()`
  retained a blank line and selected Alpine `build-base`, stopping after four
  base rows before C coverage. This harness blocker remains open.
- The previous alternate-stack thread fixture stopped at `pthread_create`:
  guest `clone3` returned `EINVAL`. No alternate-stack coverage is claimed.
- Existing `ldxp-stlxp.c`, built `clang -O2 -static -march=armv8.1-a+lse`,
  terminated with native SIGBUS (135) during engineering validation. It is not
  counted as a guest pass or an emulator differential.
- Existing nearby-page read-fault recovery can silently map readable zeros.
  The new fixture deliberately unmaps both pages of an isolated region to
  receive a real signal. It tests a first-page fault for a split load, **not**
  second-page-only faults or general `PROT_NONE` enforcement. The workaround
  was documented, not changed or accepted as Linux-conformant behaviour.
- No complete language/runtime matrix, sanitizer, fuzzing or exhaustive ISA
  conformance result is claimed. Older results remain scoped to their revisions.

## iOS validation still required

No macOS cross-build, Xcode archive, signing, installation, simulator,
physical-device test or App Store upload was performed. Before distribution:

1. Build/sign both intended ARM64 schemes on macOS and verify version/build.
2. Exercise precise load faults, signal return, CBZ/CBNZ load fusion, split loads
   and repeated computation on the exact device archive.
3. Test foreground/background transitions, memory pressure and thread creation;
   investigate the known libc/`clone3` issue for the packaged rootfs.
4. Retain seek, poke and FP regressions from previous releases.
5. Measure device responsiveness, thermals and battery use independently of Linux.

The inherited Fastlane upload lane still targets upstream iSH. A pushed source
tag is not evidence of a GitHub release object, signed archive or store release.

# ios-linuxkit 2.3.3 / build 816 — 1 October 2026

Patch source release based on `ec2d0846` (2.3.2/build815). This repairs a real
BusyBox gzip compatibility failure and adds instruction and file-cleanup gates.
It is not a signed iOS binary or App Store upload.

## Root cause

Frozen Alpine 3.24.2 BusyBox 1.37.0-r31 compression failed in the gadget release,
the isolated memory prototype and AOT mode at guest PC `0xeff7b5a4`, instruction
`0x7e7f2fde`. The same BusyBox runs successfully natively. Its ELF offset
`0x6e5a4` contains valid `uqsub h30, h30, h31`, followed by another scalar UQSUB
at `0x6e5c0`. The gzip match-table slide subtracts the 32 KiB window size from
16-bit positions, clamping underflow to zero. This is ordinary buffer bookkeeping,
not corrupt guest code, an allocation failure or an invalid instruction.

The AdvSIMD scalar three-same decoder omitted saturating add/subtract. Vector
forms already existed; scalar UQSUB therefore fell through to guest SIGILL.
No ROM, BusyBox binary, corpus or guest memory layout is patched.

## Repair and scope

- Decode scalar SQADD/UQADD/SQSUB/UQSUB for all B/H/S/D widths in the existing
  scalar three-same class; unrelated/reserved opcodes retain their existing path.
- Dedicated precompiled gadgets load both source registers before storing,
  support aliases, clear every inactive destination bit, and update cumulative
  guest FPSR.QC using the actual scalar instruction. Inactive lanes cannot set QC.
- Save/restore host FPCR/FPSR around execution; guest NZCV remains unchanged.
- Add focused scalar and native/guest gzip regressions and Make targets.

This is part of failure/cleanup and memory correctness work: it repairs gzip's
buffer-index instruction and checks bad-input handling and file lifecycle.
It does not implement the broader offload allocation/thread rollback, memory
sampler, fork admission or page-table OOM work. No prototype branch is merged.
Native/AOT stays default-off; app schemes, Alpine pin, AOT code version/ABI,
netlink opt-in and memory admission defaults are unchanged.

Version sources: `MARKETING_VERSION=2.3.3` in `app/AppARM64.xcconfig`, four
`CURRENT_PROJECT_VERSION=816` project entries. Annotated source tag `v2.3.3`.

## Host and frozen inputs

Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four Cortex-A520
cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, host-native Debian
Trixie, Linux 6.6.89-cix AArch64, 4 KiB host pages. Clang 19.1.7, Meson 1.7.0,
Ninja 1.12.1, Bun 1.4.2. Separate gadget and no-emitter AOT release/debug builds.
Native restart testing uses an additional recorder build. No Apple SDK/device.

Frozen restored guest from the verified AOT seed; no package fetch or retraining.
BusyBox SHA-256:
`97d52efa149563c8d886e3670e2496d4140d3c54138017afd3a105e0397fae2e`.
Original 674,041-byte failing text SHA-256:
`f29f92367d2b2e81f4631947c09a98cfd595c80c0bbcf05f73f379a90d87c6bb`.
The tracked runner regenerates these same text bytes deterministically.

Evidence root: `/workspace/artifacts/ish-gzip-fix-20261001/`.

## Gates

- Baseline gzip replay exits 1 with the same illegal instruction. Baseline scalar
  fixture also rejects its first SQADD. Candidate gzip round-trip succeeds.
- Scalar native integer oracle: 19,696 cases per build, all four operations and
  widths, min/max/overflow/underflow, distinct/left/right/source/all aliases,
  full 128-bit result, clear/sticky QC, preserved FPSR exception bits and NZCV.
  Includes exact high-register gzip instruction. Candidate output equals native.
- GCC UBSan checks the independent `__int128` arithmetic oracle without errors.
  Clang's optional UBSan attempt could not link: host compiler-rt archive absent;
  the GCC sanitizer run is the passing evidence.
- Gzip release/debug: 33 checks each. Nine deterministic inputs including empty,
  one byte, repeated bytes and 32/64 KiB boundaries; levels 1/6/9 bit-identical
  to the same BusyBox executed natively through its matching musl loader.
  Host inflate checks every result. Guest inflates a host-produced stream,
  rejects truncated/CRC-corrupt streams with matching native status, and checks
  failed-decompression archive/partial-output state against native BusyBox as well
  as successful compression/decompression input removal.
- Gadget release/debug upstream correctness, actual-kernel anonymous accounting,
  injected mapping/CoW failure and lifecycle gates.
- Focused FCVT vector, exact load-PC, proc-memory seek, lseek width, regular-file
  poll and internal-continuation 14/14 gates in gadget and AOT release/debug.
- Full procfs stress: native control and two guest repetitions, each 25 s,
  16 forkers and six readers, with progress and worker-shutdown checks.
- Frozen AOT release/debug: all four images accepted/used, runtime-off output
  parity and no emission. Gzip with AOT enabled round-trips and reports zero
  unsupported instructions, zero emission and positive AOT hits.
- Native O0/O2 exact restart/oracle harness, including 8,576 integer and 2,720
  memory cases per mode. Generator/kit tests: 13 tests and 65 assertions.
- Netlink release/debug: 211 checks plus interface-binding tests, unchanged opt-in.
- Documentation link/style gates and `git diff --check`.

Failed harness attempts remain in evidence: initial native BusyBox invocation
used fakefs data files without host executable permissions; the runner now makes
executable byte-identical copies for the native oracle. One preliminary debug
corpus differed from the frozen text; final regenerated corpus hashes match in
both lanes. Initial netlink invocation omitted its required disposable realfs;
corrected invocations use independent roots. No failed attempt is counted as a pass.
An initial release procfs run timed out with no guest log; the failure is retained
and its clean-root recheck passes both guest repetitions at the original timeout.
The exact cause of that first stress timeout is not diagnosed; it is not erased
or claimed as an opcode failure.
A delegated fixture task timed out at 180 s without producing files; the fixture
was implemented locally and no independent-review claim is made.

## Reproduction

Use a disposable restored guest, not a user's installed filesystem:

```sh
CC=clang make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-2.3.3-gadget-release
CC=clang EVIDENCE_DIR=/absolute/new-scalar make test-arm64-scalar-saturation \
  RELEASE_BUILD_DIR=build-arm64-2.3.3-gadget-release ROOTFS_DIR=/absolute/test-root
CC=clang make test-arm64-gzip RELEASE_BUILD_DIR=build-arm64-2.3.3-gadget-release \
  ROOTFS_DIR=/absolute/test-root REPORT_DIR=/absolute/new-reports
```

Repeat with a fresh debug directory and matching libraries. Retain binary/guest
hashes, return codes and output comparisons. The gzip runner removes inherited
JIT/AOT/trace overrides and tests gadget mode; separate linked/AOT checks exercise
the existing no-emitter configuration. There is no performance claim.

Apple archive, signing and physical-device validation remain separate. This patch
fixes execution through precompiled gadgets, without enabling runtime code
emission or experimental app AOT/memory/offload paths.

# ios-linuxkit 2.4.1 / build 818 — 2 October 2026

Patch source release prepared from 2.4.0/build817. Includes the validated
default-off offload tranches through `9c41fdbd`, local-copy/VFS repair `9c367462`
and shared recovery/layout preparation `5a01295c`. No signed Apple archive,
App Store upload or physical-device validation is recorded.

## Changes

- Transactional legacy offload setup: checked allocations, pipe/descriptor and
  forwarder ownership, spawn actions, precommit rollback and child reap. Legacy
  argument/CWD/forwarder behaviour remains separate from the cooperative API.
- Startup-only cooperative handlers: raw guest argv, retained guest root/CWD/
  umask and guest VFS opens, sibling/exiting-group refusal, no framework workers,
  chdir, path rewriting or metadata scans.
- Restricted connected INET/INET6 TCP streams: per-call nonblocking operations,
  4 KiB calls, separate 1 MiB input/combined-output budgets, 250 ms retries and
  5 s checkpoints. Unsupported terminal/file/pipe/Unix/datagram stdio is refused.
  Locked token cleanup and signal/fork isolation preserve pending-signal handling
  and SIGKILL priority. These limits do not provide a universal I/O watchdog.
- Test-only local-copy example: at most 1 MiB/1,024 transfer calls, exclusive new
  output, final-symlink refusal and exact partial-output retention. It is never
  linked/registered by normal apps or CLI builds and does not integrate FFmpeg.
- Realfs and fakefs direct bind opens reserve descriptors before backing open/
  create/truncate. Allocation refusal returns ENOMEM without a NULL dereference
  or backing-file side effect; failed opens release the reserved descriptor.
- Shared fault core with separate CLI/callable app adapters. The CLI preserves
  its gadget replay/signal policy; the uninstalled app adapter permits only exact
  native checkpoints. Unmatched native faults fail closed. No app scheme installs
  the adapter or enables native images.
- Read-only `jit_layout_read`/JSON and native-only `/proc/ish/jit-layout`: compiled
  layouts without init/allocation/code mapping; convention fields only after
  normal initialisation reports `ready=1`. No recording relabelling.

Maintained documentation is refreshed before tagging: README/security, all
12 existing guides, new [offload contracts](../../NATIVE_OFFLOAD.md), current
indexes, release procedure and Apple handoff. Historical reports, upstream and
vendored notes retain their original evidence. Current commands reference 2.4.1;
Apple contract observations remain separate from Linux values.

Version sources: ARM64 `MARKETING_VERSION=2.4.1`; all four project
`CURRENT_PROJECT_VERSION=818` entries. Annotated tag `v2.4.1` is created only
on the clean, validated release commit.

## Defaults and exclusions

Gadget execution remains the default. Existing Xcode schemes force `jit=false`,
`jit_emit=false` and empty CLI images together. The legacy fake FFmpeg test
scheme is unchanged; no production cooperative handler, real FFmpeg, terminal
stream admission or app AOT scheme is enabled. No memory-pressure admission,
allocation tickets or clustering is imported. The CoW waiting protocol, opt-in
netlink switch, Alpine 3.24.2 pin and frozen Linux AOT ABI `3f650e41` are unchanged.

Checkpoints cannot bound a disk or Apple UI operation that does not return.
Partial output is not a committed result. Same-size source mutations, atomic
publication, durability, full guest permission conformance and hostile-workload
isolation are outside the local-copy contract. Ready diagnostics do not prove
image acceptance or an Apple binary's ABI/signing/device behaviour.

## Host and evidence

Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four Cortex-A520
cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, native Debian Trixie,
Linux 6.6.89-cix AArch64, 4 KiB host pages; Clang 19.1.7, Meson 1.7.0,
Ninja 1.12.1 and Bun 1.4.2. No Xcode/Apple SDK/signing/device environment is
available. Source validation does not establish an iOS archive or distribution.

Prior tranche evidence and failed development attempts are retained under:

- `/workspace/artifacts/ish-offload-bounded-io-20261002/`;
- `/workspace/artifacts/ish-offload-local-copy-20261002/`;
- `/workspace/artifacts/ish-shared-recovery-20261002/`.

Fresh release evidence is under
`/workspace/artifacts/ish-2.4.1-release-20261002/`. Separate fresh gadget and
compile-time no-emitter AOT release/debug builds use restored copies of the
frozen Alpine 3.24.2 seed and four existing ELF images, without package fetch or
retraining. Runtime source and runners were frozen and hashed before launch;
source hashes were verified after the tail gates. Final docs have a separate
post-evidence hash record.

All four lanes pass:

- configuration/build and five actual-source offload gates: 102 local-copy
  cases, 39 TCP dispatcher modes, 1,000 token/signal races, context/filesystem
  semantics, seven executor allocation rollbacks and legacy setup injection;
- upstream actual-archive correctness, lifecycle, OOM/accounting, CoW/ptrace,
  signal-frame, waiting-protocol/real STP and shared CLI/app adapter/layout tests;
- full procfs stress: native plus two guest repetitions, each 25 s with 16
  forkers/six readers, required progress and verified worker shutdown;
- FCVT, precise load PC, proc-memory/full-width seeks, regular poll, CPU poke,
  14/14 continuation and 19,696 scalar saturation cases;
- corrected route-netlink 211 checks and interface binding each.

Gadget release/debug pass all 33 frozen BusyBox gzip bitstream/inflate/error/
partial-file/lifecycle checks. Both no-emitter AOT lanes accept/use four frozen
images with runtime-off parity, zero emission and no emitter imports. Native
O0/O2 tests each pass 110 actual restart cases (including the callable app
adapter), fail-stop rejection, 8,576 integer and 2,720 memory oracle comparisons,
family/slot ownership and before/after-init read-only layout checks. Documentation
links/style/tests, rootfs packaging, Xcode gadget guard, generator and kit pass.

The first netlink compile failed in all lanes before execution because the
Debian musl wrapper omitted Linux UAPI headers. Those `netlink.rc=1` and initial
`all.rc=1` files are retained. Corrected `netlink-corrected.rc=0` runs use
`-idirafter /usr/include -idirafter /usr/include/aarch64-linux-gnu`; no runtime
source change was needed. The initial failure stopped linked/gzip tail gates;
`run-tail-gates.sh` then completed those missing gates at 16:00:23 UTC with
`tail.rc=0`. `acceptance.json` verifies 54 authoritative gate statuses rather than overwriting
failed results. The final source ledger covers the exact release tree; all runtime
sources, runners, frozen inputs and built binaries match their tested hashes.
Four post-run documentation updates are explicitly separated from the initial
frozen-source ledger. The networking guide records the include-path requirement. A delegated documentation audit timed out after
120 s; no independent-review claim is made.

## Apple gates

Build both distributed gadget schemes with Xcode and smoke-test their exact
signed archives before Apple distribution. An accelerated app separately needs
an isolated no-emitter scheme, installed fault adapter, observed target ABI,
matching Mach-O images, constructor/import/mapping/signing checks and physical-
device fault/CoW/invalidation/lifecycle/stale-image fallback tests. Rebaseline
correctness, timing, RSS, app size, thermals and energy before choosing images;
evaluate musl/BusyBox first and include Python/zlib only if justified. Keep
accessible gadget rollback. Those gates are not closed by this source release.

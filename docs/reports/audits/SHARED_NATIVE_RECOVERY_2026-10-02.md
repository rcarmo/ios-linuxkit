# Shared native fault recovery preparation — 2 October 2026

## Disposition

Prepared on `prepare/app-aot-recovery` from the published local-copy tranche
`9c367462`. No app handler is installed, no AOT scheme is created and no existing
scheme's gadget defaults change. The app adapter is a callable preparation API,
not an integrated Apple signal/Mach exception path. Master remains `9c41fdbd`.
Source release remains 2.4.0/build817; no memory-policy/clustering imports.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 CPU cores, 16 GB-class RAM
(about 14 GiB visible), NVMe storage, Debian Trixie AArch64 with 4 KiB pages,
Clang 19.1.7, host-native execution. Neither `xcodebuild` nor `xcrun` is present.
This proves only the Linux contracts below.

## Shared fault core and separate adapters

`platform/native_fault.c` extracts the existing CLI recovery path into
`ish_native_fault_recover`: UNHANDLED, REDIRECTED or FATAL. It performs no handler
installation, allocation, locks or signal-mask change. Generated assembly offset
assertions move with the core, including a CPU-at-frame-base assertion.

The existing native checkpoint recogniser remains unchanged: exact host PC,
dispatch's trusted thread-local frame and matching x1 are required. It supplies
exact guest PC/address/write state and disarms the checkpoint. Unmatched native
JIT or AOT text is FATAL, never gadget/block replay. The common core changes only
fault/diagnostic fields and host SP/PC for the existing trampoline; FP/SIMD and
other host signal-context state are untouched.

The CLI adapter keeps its existing gadget x1/x7/x10 recovery, precise-slot/TLS
fallback, signal-unblock policy and diagnostic exit 139. The callable app adapter
`platform/native_fault_app.c` never admits gadget replay. Without ISH_JIT it
returns UNHANDLED without examining the context. Under ISH_JIT it can redirect
only an exact native checkpoint; its future caller must fail-stop on FATAL and
preserve its prior policy on UNHANDLED. Existing UIKit entry points do not call
or install it. Darwin/arm64e context, signal-mask lifecycle and device crash
reporting still need their actual SDK and application integration.

## Read-only target diagnostics

`jit_layout_read` returns the compiled sizes/offsets and compile-time emission
setting. `jit_layout_describe` serialises bounded JSON. Neither invokes init,
reads configuration, allocates, maps executable memory, changes pinning or
registers images. The native-build-only `/proc/ish/jit-layout` is mode 0444 and
has no update/pwrite callback. It is absent from gadget builds.

Convention fields are zero with `ready=0` until normal backend initialisation
has selected PIC/pinning. A release/acquire atomic publication protects those
immutable fields. A no-emitter bootstrap with no images may remain not ready:
that is not a target contract and must not be replaced with recording-header
values. Readiness describes selected conventions, not whether images were
accepted or the backend is executing. An invalid image is still rejected.

The tested Linux no-emitter PIC/pinned contract is unchanged: ABI `3f650e41`,
code version 10, 20 prologue words, entry offset 76, 16 pinned registers, 64-bit
pointers. The frame is 33,776 bytes; checkpoint offsets are 33,736/33,744/33,752/
33,760. These are Linux observations, not Apple contract values. Binary SHA-256,
SDK/platform/endian and evidence binding remain the generation tool/operator's
responsibility. No recording is relabelled and no rejection is bypassed.

## Verification

Evidence: `/workspace/artifacts/ish-shared-recovery-20261002/`.

- Four lanes (gadget release/debug and no-emitter AOT release/debug) pass build,
  local-copy, TCP/token, context, filesystem, setup, upstream, full proc/exit and
  compatibility gates; both frozen linked-AOT gates pass. Broad runner completed
  08:28:39 UTC. Source hashes are retained.
- Eight CLI synthetic contexts cover SIGSEGV/SIGBUS, read/write, precise/fallback
  PC and exact byte footprint. App adapter returns UNHANDLED without changing
  frame/context/mask on gadget faults; malformed x1 on an armed native checkpoint
  is FATAL without committing state. The final four-lane upstream rerun passes.
- Actual Linux faults at O0 and O2 exercise 100 existing emitter restart cases
  plus ten app-adapter cases (read/write across unpinned, pinned, promoted-loop,
  PIC/AOT relocation and separate-allocation chaining). Exact retry preserves
  prefix side effects, register/FP state, address/writeback and bounded TLB retry.
  Both CLI and app adapters fail closed on unmatched native/AOT PCs. These are
  Linux SA_SIGINFO tests, not Apple-context emulation or device evidence.
- Each optimisation runs 8,576 integer and 2,720 memory native-oracle comparisons,
  saturation rejection, accepted SIMD and family/slot ownership probes.
- Layout tests before/after normal no-emitter init cover empty/off/rejected-image
  cases, NULL/tiny buffers, repeated equality, exact offsets and no allocation or
  mmap from diagnostics. Actual procfs tests show read-only rejection and gadget
  endpoint absence. A linked guest reads valid JSON with the unchanged contract.
- The first proc-layout fixture compile lacked kernel errno declarations; the
  failure status is retained. Corrected four-lane tests pass. No independent
  delegated review is claimed (previous tranche review attempts timed out).

A final compile-only assertion and endpoint-fixture integration were followed
by fresh four-lane builds/upstream and O0/O2 native tests. No generated-code ABI
or fault algorithm changed after the broad gates. Final source hashes record
that distinction instead of treating earlier runner output as new evidence.

## Apple handoff boundary

Shared-source preparation is complete only at Linux-host scope. Actual app
adapter installation remains gated on an isolated Apple no-emitter scheme,
matching ISH_JIT definitions, observed ABI/layout, matching Mach-O images and
verified constructors/imports/mappings/signing. Preserve x18/arm64e conventions,
real fault-context and mask policy, exact checkpoint lifetime and fail-stop
behaviour. Then prove physical-device fault/CoW/invalidation/lifecycle/stale-image
fallback gates and correctness/timing/RSS/app-size/thermal/energy rebaseline.
Evaluate musl/BusyBox first; Python/zlib inclusion remains a measured decision.
Existing gadget-only schemes and rollback must remain available. The whole
plan is not complete without these Apple gates.

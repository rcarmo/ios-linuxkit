# Portable admission / Linux follow-up — 2026-11-15

Post-2.6.0 source tranche, not a new app release. Baseline: published
`b8e8cb72` / `v2.6.0`, build 827. No tag, app version or default execution mode
was changed. Maintained contract: [MEMORY_ADMISSION.md](../../MEMORY_ADMISSION.md).

## Target and boundaries

Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 ARM64 CPU cores, 16GB-class RAM
(`MemTotal` 15,882,027,008 bytes), NVMe storage; Debian Trixie, aarch64 Linux
6.6.89-cix, 4096-byte host pages, cgroup v2. Host-native agent service remains
under `piclaw.service`; `/workspace` resolves to `/home/agent/workspace`.
Clang 19.1.7 and GCC 14.2.0 were used. Builds were bounded to `-j2`.

No Apple SDK/device, host pressure settings, governor, cache, swap or global
limits were changed. Pressure tests used new delegated user-service cgroups,
not Piclaw's cgroup. No Rust builds. No Apple validation or jetsam claim.

## Implemented

- Portable default-off tickets separate logical anonymous guest pages from
  host-page-rounded allocation debt. Pending reservations survive through
  cleanup. Positive protection upgrades, materialized lazy/stack pages and
  CoW/ptrace copies use the same admission mechanism.
- Successful debt belongs to shared `struct data` until its final successful
  host unmap. Samples and enable/disable cycles cannot settle untouched backing.
  Failed rollback unmaps refund logical pages but quarantine host debt for the
  process lifetime. Conservative double counting is intentional.
- Protection upgrades admit before charging; failed host protection refunds.
  Each successful page and reservation mutation publishes memory/TLB state
  before a later failure can return. Existing CoW/ptrace/signal lock revalidation
  and seq_cst TLB invalidation remain intact.
- `brk` growth no longer duplicates the central allocator's charge. Shrink and
  failed growth now balance the same ownership.
- Linux sampler resolves membership/mount roots and all visible cgroup-v2
  ancestors, uses each ancestor's own usage with its own limits, combines host
  availability and PSI, rechecks identity/limits, and fails closed on invalid
  observations. A process-lifetime worker is explicit `ISH_MEMORY_PRESSURE=1`
  opt-in, with a 64MiB reserve and 250ms interval. An older failed feed cannot
  invalidate a newer capture.
- Meson includes the shared core everywhere and Linux adapter only on Linux;
  legacy Xcode source membership includes the core. Actual Apple compilation
  remains deferred. GNUstep `plutil` here could not validate the project file;
  source-reference inspection is not an Apple build result.
- Portable clustering selection contract is retained and tested, but **not
  wired into allocation**. Real 4KiB geometry selects one page; synthetic 16KiB
  selection checks do not establish an allocation or performance benefit.

## Regressions established before repair

`/workspace/artifacts/ish-portable-policy-20261115/` contains receipts.

| Regression | Before | After |
|---|---|---|
| Protection upgrade over logical cap | abort 134 on published 2.6.0 archives | admission refusal, exact charge and recovery |
| Later host protection fails | abort 134: earlier change not published, published archives | committed prefix published; failed page refunded |
| Reservation split fails after earlier mutation | abort 134 on then-current candidate | committed reservation prefix published |
| `brk` growth/shrink | abort 134: expected 2 pages, got 4, unchanged baseline brk path | one central charge, shrink/refusal balanced |
| Failed allocation plus failed rollback unmap | abort 134 on pre-quarantine candidate | logical refund without fictitious host credit |

The latter three are pre-fix candidate receipts, **not relabelled pristine-tag
runs**. Separate first-page failure, rollback, cap refusal and recovery checks
also pass.

## Final Linux gates

| Gate | Result |
|---|---|
| Policy/protection/accounting/geometry | 45 executable invocations per release/debug/sanitized lane, all pass |
| Explicit admission enabled | CoW/ptrace 9 modes, signal-frame 4 modes, lock-upgrade 25 modes; included above |
| Actual-core ticket tests | concurrent claims; retained/pending debt; shared-backing attachment; replay/future/stale captures; hysteresis; invalidation; mmap/data/protection/cleanup failures; all pass |
| Upstream actual-kernel and guest matrix | release and debug pass, using separate disposable Debian roots |
| Linux sampler fixtures | 71 distinct checks at O0 and O2; Clang/GCC builds, GCC sanitizer and live reads pass |
| Disposable cgroup bridge | 10 capacity/failure/recovery scenarios, tested with 0/4/8MiB reserves; final run adds worker ownership/join/EBUSY scenario: 11 pass |
| Cgroup events | no OOM/OOM-kill; physical sibling pressure 96MiB; final unit peak about 97.3MiB, zero swap |
| CLI | `0` and `1` run `PRESSURE_CLI_OK`; invalid value exits 2 |
| Native/AOT emitter | 11 pass, zero failures, 137 assertions; ABI `3babd760`, code version 15 |
| Exact gadget profiling | ordinary weighted 11 / patches 2+2; exact weighted 30 / patches 0+0 |
| Linked no-emitter | five images used, zero rejected, zero runtime emission, runtime-off parity |
| Imported correctness gates | poll, socket ownership, SHA512 fallback, stale-TLB / CoW-store pass |
| Cooperative offload | 39 I/O dispatcher modes; 1000 cancellation races; real/fake context and local-copy fixtures pass |
| Held-out harness | 7 tests / 18 assertions; final evaluation 24 measured executions, exact output oracles pass |
| Logical cap disabled | selected core memory/mmap/policy C files syntax-check with `ANON_MMAP_LIMIT_PAGES=0`; not a complete no-cap runtime lane |

Policy invocation count: protection 4, anonymous accounting 1, ticket core 1,
CoW 9, signal 4, upgrade 25, geometry 1. This is not 45 independent algorithms.
The sampler's fixture checks are repeated across compiler/configuration lanes,
not counted as additional distinct scenarios.

### Sanitizer scope and failed attempts

Final project C build uses GCC ASan+UBSan, Clang for existing LLVM-specific
assembly, and `-latomic` for GCC's 16-byte CAS reference. Executed policy gates
use matching sanitizer flags with `UBSAN_OPTIONS=halt_on_error=1` and
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`. No sanitizer diagnostic occurred
in the final executed cases. Project configuration disables alignment sanitizer
for the production C lane; harness C retains its own flags. Assembly, gadgets,
external system libraries and emitted instructions are not instrumented.
Leak detection is disabled; this is not a whole-program leak/UB proof.

Earlier attempts failed: pure GCC assembly rejected existing register aliases;
the mixed link initially lacked `__atomic_compare_exchange_16`; GCC's signal
fixture diagnosed longjmp-clobbered argument state (fixture refactored before
passing). Clang ASan runtime archives are absent on this host. No failed attempt
is counted as passing.

An upstream run using a shared guest root failed the open/unlink race while
release/debug ran concurrently. Final runs use distinct disposable roots;
the race fixture now creates a unique directory per invocation while retaining
eight contending workers. Reusing existing fakefs offload evidence directories
also failed fixture creation; final offload runs use fresh evidence directories.
These failures were resolved, not skipped. `NINJA='ninja -j2'` is now unexported
from Make's child environment so Meson does not interpret a command string as an
executable pathname.

## Held-out baseline and fusion decision

Details: [held-out qualification](HELDOUT_LINUX_2026-11-15.md). Final manifest:
`aca04fed812da0c00cd2d4d60ab5c4836014dcdf05d800b35e588267ce434462`.
Raw alternating paired samples and inputs are retained under
`heldout-complete-final/`, with configuration `heldout-config-full.json`.
Host runner: `/usr/local/bin/bun` 1.3.10; **guest** Bun is separately verified
1.4.4. Guest staged evaluation hashes match the host input hashes. Training
uses verified ABI15 recordings, not the stale ABI in the initially suggested
artifact directory. No training on evaluation results.

Medians, including startup, two samples per emulator mode (small smoke baseline,
not controlled performance proof):

| Guest workload/lane | Wall seconds | Peak RSS KiB | Minor faults | Major faults |
|---|---:|---:|---:|---:|
| Python gadget | 1.485 | 25,900 | 4,847.5 | 0 |
| Python linked no-emitter | 2.070 | 51,008 | 5,536 | 0 |
| Bun gadget | 0.915 | 60,352 | 7,251 | 0 |
| Bun linked no-emitter | 1.305 | 103,862 | 8,104 | 0 |
| Go gadget, frozen static smoke executable | 0.120 | 26,688 | 5,801.5 | 0 |

Go also runs natively, but its elapsed time is below GNU time's displayed
resolution; **no Go AOT image or arbitrary-Go acceleration is claimed**. Native
Python/Bun/Go baselines and their raw faults/RSS are retained. Peak RSS is the
host process maximum, not guest-only footprint or allocation delta. Short
uncontrolled runs do not establish an admission-policy performance benefit.
Linked Python/Bun are slower and larger here: no speedup is inferred.

A separate final profile uses **only the frozen training workload**, recording
its and the executed binary's hashes in `profile-training/identities-final.sha256`.
Training oracles pass, zero samples are dropped. Ordinary weighted dispatch
profiles are not exact instruction counts. Existing compile-selection counters
include `addsub_fast=305729`, `cmp_bcond=107783`, `addsub_cbz=6`; these are not
causal application timing evidence. The missing DECREF/INCREF gadget set and
its dependent guard are not imported. No complete new candidate with measured
gain is established; **the fusion gate closes with no new fusion port**.

## Deferred, not prerequisites

Darwin/iOS sampler mapping, unsupported-data policy and lifecycle requirements
are specified in the maintained guide. Native adapter implementation, 16KiB
host protection behaviour, signing and app/device lifecycle validation remain
deferred as authorised. No Linux result is labelled Darwin validation.
Clustering optimisation remains deferred without appropriate real geometry and
measured benefit. Current cancellation machinery is reused rather than
re-importing the prototype's obsolete handler ABI.

## Evidence map

Root: `/workspace/artifacts/ish-portable-policy-20261115/`.

- `before-{cap,partial,first}.log`, `brk-before.log`,
  `reservation-before.log`, `rollback-before.log`.
- `policy-release/`, `upstream-release/memory-policy/`,
  `upstream-debug/memory-policy/`, `policy-sanitizer/`.
- `upstream-{release,debug}.log`, `native-gates.log`, `import-gates.log`,
  `linked-final/`, `linked-gates.log`, `sampler-sanitizer-final.log`.
- `cgroup-final/`, `cgroup-earlier/`, `make-cgroup-final.log`, `cli-*.log`.
- `offload-io/`, `offload-context-verified/`, `offload-local-copy-verified/`.
- `heldout-complete-final/`, `heldout-summary.json`,
  `{host,guest}-input-hashes.log`, `profile-training/final.{profile,log}`.

Earlier failed held-out attempts remain labelled FAIL. One extra Go case
initially used fakefs's inner `data/` directory instead of its outer root;
the corrected configuration passes. No failed recording/run was relabelled.

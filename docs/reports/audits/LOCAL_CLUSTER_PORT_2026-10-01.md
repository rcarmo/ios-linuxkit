# Bounded local clustering port — 1 October 2026

## Scope

Prototype branch `prototype/memory-pressure-policy`, based on `db90a2af` and the
explicit-ticket allocator in `1d0ab36a`. This is not on release/master. Both the
pressure policy and clustering remain disabled by default; no app, CLI or sampler
activates them.

The upstream [real-code probe](REAL_CLUSTER_SPAN_PROBE_2026-09-30.md) passed its
ownership controls but exposed backing leaks when data-object allocation failed.
This port adopts only cluster selection: it retains our `pt_map_nothing()` and
explicit ticket ownership, not upstream allocation/accounting or TLS precharge.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 CPU cores, 16 GB-class RAM
(about 14 GiB visible), NVMe, native Debian Trixie AArch64, Clang 19,
4 KiB actual host pages. Synthetic 16 KiB allocation spans are test scaffolding.

## Implementation boundary

- `struct mem.cluster_commits` defaults false in `mem_init()` and is inherited
  by fork/CoW. It is an internal, write-lock-owned prototype setting; exec creates
  a fresh disabled address space. There is no runtime configuration switch.
- `pt_map_cluster()` permits at most four aligned guest pages (one host span,
  no larger than 16 KiB). It requires an explicit ownership predicate. A missing
  predicate, disabled setting, occupied neighbour, unsupported geometry or
  incompatible page selects only the faulting page. An already mapped fault
  returns `_EINVAL` without replacing it.
- Both cluster and single-page retry use the same ticket-backed map primitive.
  Failed backing installation unmaps its allocation before refunding the pending
  ticket; no new claimant can consume the slot while unused backing remains live.
- Lazy-fault clustering requires the same reservation for every neighbour. The
  reservation is re-fetched after the read-to-write lock upgrade, rather than
  dereferencing a pointer another thread may have freed/split/replaced.
- Reservation ownership takes precedence over an unrelated growsdown mapping.
  A `PROT_NONE` reservation is not materialised by a write fault.
- Stack clustering may fill unmapped pages between the requested fault and the
  revalidated existing stack anchor. It never grows below the fault, consumes a
  reservation or extends the prior RLIMIT check. A mid-span fault falls back to
  one page. No emergency allocator bypasses pressure admission.
- The existing heap-gap read recovery in `kernel/calls.c` remains single-page.
  This port does not import upstream's predicate-free clustering for that path.
- Mapping, unmap, protection and CoW continue to publish generation changes;
  allocations remain shared by `struct data` until the final guest owner unmaps.

## Actual-kernel ownership and integration gate

[Fixture](../../../tests/arm64/upstream/cluster-span.c) and
[runner](../../../tests/arm64/upstream/run-cluster.sh) link actual candidate
archives. Linux wrappers align and round host backing to 4/16 KiB, track live
allocations and inject map/data-object failure; they do not implement clustering.

**46 cases pass per lane** in gadget release/debug and linked no-emitter AOT
release/debug (184 case executions). Cases include:

- 4/16 KiB aligned offsets, partial/final unmap and balanced host allocation;
- reservation boundaries, differing flags, occupied neighbours and hole reuse;
- fork/CoW independent child contents, final shared-span ownership;
- permission changes, denied stores and generation publication;
- default-disabled behaviour, no-predicate fallback and refusal to replace an
  already mapped fault;
- real TLB cached-pointer rejection after replacement while old backing remains
  shared, and cached writable-entry rejection after permission reduction;
- actual lazy faults, `PROT_NONE` refusal, failed cluster installation fallback;
- deterministic competing writers freeing or replacing the reservation with
  read-only flags between read-unlock and write-acquisition;
- twelve simultaneous lazy writers, bounded stack growth and mid-span fallback;
- hard logical-cap fallback, critical-pressure refusal and fresh-feed recovery;
- actual `handle_interrupt(INT_GPF)` lazy/stack resolution, preserving guest PC
  and registers;
- single and double host-map/data-object failure, zero live backing and pending
  debt at completion. The failed allocation remains ticket-covered until unmap.

The two upstream leak cases now pass: one data-object failure produces two maps
and two unmaps; double failure also produces two maps/two unmaps and no commitment.
The fixture checks host ownership separately from the logical guest-page ledger.

```sh
make test-arm64-cluster CC=clang RELEASE_BUILD_DIR=build-memory-policy-release
BUILD_DIR="$PWD/build-memory-policy-debug" CC=clang \
  EVIDENCE_DIR=/absolute/evidence bash tests/arm64/upstream/run-cluster.sh
```

The existing upstream gate invokes the cluster runner automatically.

## Broader gates and retained evidence

Evidence: `/workspace/artifacts/ish-clustering-local-20261001/`.

Four lanes build from this candidate: `build-memory-policy-release`,
`build-memory-policy-debug`, `build-memory-cluster-aot-release` and
`build-memory-cluster-aot-debug`. AOT lanes link the four frozen ELF images from
`/workspace/artifacts/ish-2.3.2/20260930/elf/`, with emission disabled.

- All four full upstream gates pass: FMOV512, syscall52, timer/flags/signals,
  lifecycle/subms, actual accounting/CoW/signal recovery, task-start rollback,
  JIT OOM/crash context; native/AOT lanes also test refused-link invalidation.
- All four procfs stress gates pass native control plus two guest runs per lane,
  each 25 seconds with 16 forkers and 6 proc/ps readers. All four continuation
  gates pass 14/14, including exact internal-segment fault PC.
- Both linked AOT gates pass four-image use, zero runtime emission and runtime-off
  parity, using `/workspace/artifacts/ish-aot/2.3.0-prototype/seed/recordings/`.
- Native emitter/recovery tests pass at O0/O2: 100 exact restart cases and 11,296
  integer/memory differential cases per optimisation; relocated AOT/PIC and
  preservation checks pass. These complement the cluster GPF/TLB tests; they are
  not a synthetic-span end-to-end hardware execution test.
- Documentation/link/style checks and the Makefile cluster target pass.

Initial linked-gate invocations used a nonexistent records directory and failed
before workload execution; corrected invocations use the retained manifest above.
The deterministic race test initially wrapped blocking `pthread_rwlock_wrlock`,
but this tree uses `pthread_rwlock_trywrlock`; the corrected hook runs the real
competing writer and passes removal/replacement assertions. Neither failed run
is counted as a pass. Initial continuation invocations also used the wrong
rootfs variable; corrected `ROOTFS` invocations pass all four lanes. An
independent delegated review timed out after 120 seconds;
no independent-review claim is made.

## Limits and remaining work

- No Apple 16 KiB device, Darwin subpage protection, physical-footprint, jetsam or
  foreground/background evidence. Guest permission tests do not demonstrate
  independent host protection for four guest subpages.
- No page-table-node OOM repair, universal fork admission cap, or file/unrelated
  host-allocation budget. The original memory-prototype limits still apply.
- Deterministic lazy-upgrade races and concurrent writers are covered, not every
  stack/CoW/ptrace lock interleaving or JIT read-lock contention schedule.
- The linked guest suites run on Linux 4 KiB with clustering disabled. The
  synthetic-span actual-kernel fixture enables clustering explicitly. Do not
  equate those two forms of evidence with real Apple clustered execution.
- No workload RSS/speedup claim. Held-out AOT evaluation comes next, then app AOT
  wiring and cooperative cancellation subject to device/handler gates.

See [prototype API and limits](../../MEMORY_POLICY_PROTOTYPE.md).

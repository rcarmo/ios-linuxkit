# Portable memory admission and platform adapters

Experimental, default off. Logical anonymous-page accounting remains enabled as
before; no-JIT defaults, AOT exact-image identity, fallback and recovery are
unchanged. This is bounded admission for selected guest anonymous allocation
paths, **not** an RSS ceiling, OOM guarantee or iOS jetsam protection.

## Shared contracts

- `anon_page_count` counts logical committed anonymous 4KiB guest pages.
  PROT_NONE/lazy address reservations do not count. Fork duplicates logical
  charges; CoW replaces a charged mapping rather than charging it twice.
- Host allocation cost is separately rounded to `real_page_size`. Anonymous
  maps, materialized lazy/stack pages, CoW/ptrace copies and anonymous protection
  upgrades acquire a caller-owned ticket **before** allocation/protection.
- A ticket is single-owner, zero-initialized, finished exactly once. Its logical
  charge and pending bytes survive until installation or failure cleanup.
  Successful cleanup of a failed allocation refunds both; success transfers host
  debt to the shared backing. If rollback `munmap` fails, logical pages are
  refunded but the surviving allocation's debt is quarantined for the process
  lifetime. Fresh samples and mode toggles cannot erase that orphan debt.
- A fresh sample cannot prove an untouched `mmap` is resident. Unlike the old
  prototype, there is **no automatic sample-based settlement**. Debt remains on
  `struct data` through partial unmaps and fork sharing, and is released only
  after successful final host `munmap`. Protection downgrade does not release
  backing or debt.
- The first protection upgrade reserves the entire host-rounded anonymous
  allocation maximum. An atomic backing claim serializes reservation,
  `mprotect`, commit and failure rollback across address spaces; subsequent
  aliases do not duplicate this backing debt. Logical alias charges remain
  independent. This is deliberately conservative: a small upgrade inside a
  large PROT_NONE allocation can be refused against the full backing maximum.
- Under the memory write lock, protection/reservation mutations publish once
  per operation, including completed prefixes before a later failure. No-op
  operations do not publish. TLB invalidation and partial-failure semantics
  remain mandatory; see the [optimisation evidence](reports/audits/AOT_RSS_OPTIM_2026-11-16.md).
- Existing resident backing may be counted both by headroom and retained debt.
  This knowingly refuses earlier; optimising settlement requires residency or
  equivalent evidence, not merely a newer `memory.current` reading.
- Mode changes invalidate samples and refuse outstanding transactions; they do
  not erase debt attached to live backing. Disabled-mode tickets acquire no new
  pressure debt, but retain ordinary logical-cap behaviour. Disabled tickets
  avoid the global policy mutex and pin disabled mode using an atomic count;
  mode changes CAS the idle state, closing the acquisition/enable race.
- Each capture has an internal monotonic start timestamp and nonwrapping
  generation. Only the latest capture may publish, once, within two seconds.
  Invalid or absent samples refuse new positive-byte admission. An obsolete
  failed feed cannot invalidate a newer capture. Admission I/O never runs under
  a guest memory lock.
- Samples publish headroom, a matching limiting-source allowance, and critical
  pressure as one locked value. Available must not exceed allowance. Brake below
  10%; recover at 15%; critical pressure forces braking. These are experimental
  policy values, not cross-platform safety guarantees.
- `pt_set_flags` admits positive logical deltas, refunds failed host protection,
  and publishes each successful page before a later failure can return. It
  preserves current metadata, CoW, ptrace, stack and signal revalidation.

The policy does not cover every host allocation: page-table nodes, executable
AOT/JIT mappings, file-backed memory, runtime/host libraries and unrelated
process allocations are outside its admission scope. Existing `pt_map` callers
and fork accounting retain their contracts. No reservation prevents siblings or
the host from consuming headroom after a sample.

## Linux adapter

`kernel/memory_pressure_linux.c` is a standalone synchronous sampler;
`memory_pressure_feed.c` bridges it to the shared policy. The sampler resolves
unified membership against cgroup2 mount roots, walks **every visible ancestor**,
and subtracts each ancestor's own `memory.current` from its own finite
`memory.max`/`memory.high`. `high` is a proactive reclaim boundary, not an OOM
limit. The tightest observed bound competes with host `MemAvailable`.

Two bounded observations recheck membership, mount text, cgroup identity and
limits. Publication is coherent, not an atomic kernel snapshot; ABA changes,
hidden ancestors and later consumption cannot be excluded. A safety reserve is
subtracted; relevant host/ancestor PSI can force braking. Missing/ambiguous/
malformed/truncated controls, overflow, changed limits, traversal/symlinks or
stale/backward timestamps fail closed. Root controller-file absence is accepted
only under the sampler's explicitly checked root exception. Synchronous kernel
reads cannot guarantee a hard I/O deadline.

CLI opt-in, on Linux only:

```sh
ISH_MEMORY_PRESSURE=1 ./build-arm64-linux/ish -f DISPOSABLE_ROOTFS /bin/true
```

Unset/`0` starts no sampler and enables no policy. Other values fail startup.
The opt-in worker samples every 250ms with a 64MiB reserve; defaults treat PSI
`some avg10 >= 1%` or `full avg10 >= 0.1%` as critical. Failures invalidate the
sample until a later successful refresh. A stale sample stops admission even if
sampling I/O stalls. The worker lives for the CLI process; the optional stop API
joins it for embedders/tests. No app lifecycle hook or Apple worker is installed.

Use disposable delegated cgroups for tests. Never impose limits on Piclaw or
shared ancestors; never change global swap, overcommit, governors or caches.

## Darwin/iOS adapter contract — implementation deferred

Keep the same ticket/sample engine and guest accounting. A future supported
adapter must obtain native process-footprint and pressure information, identify
what its allowance means, reserve platform-specific safety headroom, use the
same monotonic capture/freshness contract, and map pressure notifications into
critical/brake/recovery state. Footprint is not interchangeable with virtual
allocation, host free pages or a guaranteed per-process limit.

If a trustworthy allowance/headroom pair is unavailable, leave the feature
explicitly unsupported/disabled; do not invent a budget or silently substitute
Linux thresholds. Sampling/notification lifecycle, foreground/background
transitions and shutdown must be tested with the actual app. No unsupported API,
entitlement or `os_proc_available_memory` availability is assumed here. Linux
fixture success proves the shared mechanism, not Darwin measurements, jetsam
avoidance, host protections, signing or device stability. SDK/device work is a
separate deferred gate, not a Linux prerequisite.

## Geometry and other reusable mechanisms

`memory_cluster.h` preserves a pure, bounded selection contract: at most one
2/4-page host span, one owner, compatible permissions, unmapped pages, range and
overflow checks, write-lock revalidation and single-page allocation fallback.
It is **not connected to current fault allocation paths**. Synthetic 16KiB
selection tests are not real 16KiB allocation/performance evidence. This host's
4096-byte pages select one page; multi-page optimisation remains deferred.

Profiling/matching, instruction-count guards, semantic/restart tests and frozen
workload oracles can be shared. Host emitters, ELF/Mach-O packaging, signing and
recording layout compatibility still require platform-specific checks. No fusion
is imported without a complete gadget/emitter/guard set and measured benefit.

Cancellation tokens, signal eligibility and cleanup ownership are already
mainlined. Reuse current guest-VFS/context/TCP APIs rather than the prototype's
obsolete raw-fd offload ABI. Linux adapter tests cover actual cancellation and
handler ownership; Apple lifecycle integration remains separate.

## Reproducible gates

```sh
make test-arm64-memory-policy NINJA='ninja -j2'
make test-arm64-memory-pressure
make test-arm64-memory-pressure-cgroup NINJA='ninja -j2'
make test-arm64-heldout-contract
BUILD_DIR="$PWD/build-arm64-linux" bash tests/arm64/upstream/run-offload-io.sh
```

See [Linux held-out qualification](reports/audits/HELDOUT_LINUX_2026-11-15.md)
and [tranche receipt](reports/audits/MEMORY_ADMISSION_LINUX_2026-11-15.md)
for exact evidence and its limitations.

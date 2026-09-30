# OpenMinis memory/AOT follow-up — 30 September 2026

Read-only comparison of OpenMinis `e6521d9c` with ios-linuxkit `c9cb3b0a`, resumed
after the route-netlink patch. These findings do not enable or import memory
changes in 2.3.2. Linux tests on the Orange Pi 6 Plus have 4 KiB host pages; they
cannot demonstrate Apple 16 KiB behaviour or protection from jetsam.

## Candidates and source

| Candidate | Upstream source | Proposed scope |
| --- | --- | --- |
| Footprint governor | `6d1178f3`, `78e96ea1`, `097492fe`; `kernel/mmap.c`, `kernel/mm.h`, `app/AppDelegate.m` | Separate opt-in host admission policy using our existing allocation ownership |
| Host-page clustering | `ce1b4b86` and follow-up history; `kernel/memory.c`, `kernel/calls.c` | Separate commit path after policy tests; do not import thread-local precharge |
| CLI governor | `75f1e775`, `main.c` | Diagnostic model only; Linux RSS is not Apple's physical-footprint allowance |
| Wider workloads | upstream benchmark/package corpus and the separately audited AOT branch | Selected independent evaluation, not blanket runtime flags |

## Preserve our accounting

Our `anon_page_count` is a logical committed **guest-page** ledger. It charges
accessible anonymous mappings, excludes `PROT_NONE`, and handles mapping
replacement, partial unmap, protection changes, lazy faults, fork/CoW and ptrace.
`pt_map_accounted(..., precharged)` transfers ownership explicitly. The real
kernel harness `tests/arm64/upstream/anon-accounting.c` injects allocation,
mapping and protection failures and checks the ledger.

The upstream footprint mode uses a process-wide sampler, 10%/15% headroom
hysteresis, a critical-pressure brake and a 2-second stale-feed cutoff. Its
admission conversion uses host page size while its accounting remains in guest
pages. In footprint mode it bypasses the ledger's admission ceiling. This is
not a drop-in replacement for our accounting:

1. The current feed stores allowance, available bytes, pressure and timestamp as
   separate atomics. A reader can observe values from different samples. A port
   needs coherent snapshots and concurrency tests.
2. Requests are checked individually against sampled available bytes. Accepted
   but not yet reflected allocations are not subtracted from that sample.
   Concurrent requests and a 250 ms sampling delay can over-admit. The logical
   ledger should remain bounded; an additional pending budget needs an explicit
   reserve/refund/settle contract before claiming strict host-budget enforcement.
3. `pages * host_page_size` is deliberately conservative for isolated 4 KiB
   commitments on a 16 KiB host, but overestimates tightly packed or clustered
   guest pages. It is not a measurement of physical footprint. Measure both
   actual allocated spans and the guest ledger.
4. Upstream's BRAKE policy permits bounded growsdown recovery but prevents normal
   lazy commits. Test signal delivery, guest stack growth and recovery carefully:
   an allocation refusal must not become a host crash or unrecoverable guest loop.
5. Startup, stale samples and return to foreground require defined behaviour.
   A missing initial sample is not the same state as an enabled sampler that has
   stopped reporting. Apple allowance APIs and actual app pressure events remain
   device-only gates.

Do not replace our ledger with the upstream thread-local precharge mechanism.
Use a policy boundary with deterministic injected samples and preserve existing
mapping rollback tests.

## Clustering hazards and tests

`pt_map_cluster()` groups adjacent guest pages into one host allocation when the
cluster is aligned, unmapped and compatible; it falls back to a single page on
conflict or allocation failure. Lazy reservations require the same reservation;
GPF/stack-growth paths have different charge rules. Shared `struct data`
refcounts keep the host allocation alive until its last guest mapping disappears.

The upstream cluster regression is an independent model, not the emulator's
actual page-table code. It is useful as a case list, not a sufficient oracle.

Before adoption, exercise actual code with injected 4 KiB and 16 KiB host-span
policies and both cluster and single-page failure paths. Cover:

- reservation boundaries and differing protections/meta-flags;
- partial unmap and hole reuse without replacing a live neighbour;
- fork/CoW and guest permission changes within a shared host allocation;
- exact map ownership, one final host unmap and no ledger underflow;
- precise JIT/AOT restart and TLB invalidation after page changes;
- strict rollback after allocation/mapping failures.

A synthetic 16 KiB policy on Linux tests decisions and ownership, not Darwin
`mprotect`, resident footprint or jetsam. Apple 16 KiB host tests must verify those
separately, including foreground/background transitions and network handover.

## Workload and app sequence

1. Prototype coherent memory policy and real-kernel failure tests, default-off.
2. Investigate clustering separately with span/refcount evidence.
3. Add held-out Python and compression/search workloads. Retain fixed guest
   package bytes, separate training/evaluation, paired timings and peak RSS.
   Existing Python AOT results are slower and use more memory; more training is
   not assumed to fix that.
4. Revisit app-side AOT build wiring only with our repaired ABI/recovery path,
   constructor retention, Mach-O generation, signing and device gates.
5. Cooperative offload cancellation needs a per-handler contract and signal/thread
   race tests. It does not make the placeholder FFmpeg offload a working port.

Shared pathname caches, reverted poll patches, blanket Node GC flags, fork
throttling and longer wait backoffs remain excluded without targeted evidence.

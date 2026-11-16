# Fresh AOT profiles and capacity-neutral block hashing — 2026-11-16

## Outcome

Fresh current-fusion images and phase-aligned Linux profiles are complete.
The retained optimisation distributes ARM64 block keys across the existing
hash table rather than wasting three quarters of its buckets on alignment.
It does not enlarge the table, change block lifetime, alter native code or
change the AOT ABI. Small alternating comparisons show improvements in five
of six runtime/backend contrasts, but **not a consistent full-AOT Bun win**.

**There is still no demonstrated AOT break-even through work count 64.**
Gadget execution remains the default. Version 2.6.0/build827, code15/ABI
`3babd760`, image checks, fallback and pressure-worker defaults are unchanged.
No release/tag or Apple/device qualification accompanies this work.

A separate precise-page-invalidation candidate reduced regeneration but
increased resident block retention. It was measured, reviewed and **rejected**;
its implementation is not in the retained runtime change.

## Environment and provenance

- Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 CPU cores, 16GB-class RAM
  (~14GiB visible), NVMe primary storage; real host pages **4096 bytes**.
- Native Debian Trixie AArch64, kernel `6.6.89-cix`; Clang19, perf6.12.74.
  `/workspace` resolves to `/home/agent/workspace`. Piclaw remains host-native
  under its unchanged user-systemd service, outside disposable test cgroups.
- Baseline source `af5031db469ce7b1c5bdcf4b2d12d2c2eede0dee`.
  Explicit `/usr/local/bin/bun` runner is 1.3.10; the other installed runner
  is 1.4.2. Neither is substituted for guest **Bun1.4.4**. Guest Python3.14.7.
- Evidence: `/workspace/artifacts/ish-aot-profile-20261116`.
  `source.txt`, `environment.txt`, `producer.sha256`, build options, binaries,
  source snapshots, manifests, raw samples and aggregation are retained.
  `summarise.ts` checks all complete series before creating `summary.json`.
- Builds use `-j2`. No governor, global cache, swap, overcommit or existing
  shared-cgroup controls were changed. Timing sets ran separately from perf
  and later stress gates. This is not a thermally/frequency-controlled study.

### Fresh training, fixed selection

All five images were re-recorded with the current ADRP+ADD fusion shape; no
old recording was relabelled. Producer SHA256:
`02c801ae794423ffb97286d5d4907524ed08b27cfa2eb9eaedca3f50bf9a7a87`.
The independently frozen ordinary training profile has `dropped=0`, SHA256
`18b72f3250d9cda87b6516e412c15324df8c8531185e5c888bbe5cae8db99154`.

Selection retains the existing training-only 90%-representation / 50%-native-
byte ceilings per image. Evaluation never updates the profile or budgets.
Both full and selected builds disable the runtime emitter and use the same
fresh recordings. Family matching is off in the measurements.

| Image | Selected / full translations | Selected / full native bytes |
|---|---:|---:|
| musl | 78 / 2,388 | 29,772 / 1,195,492 |
| busybox | 265 / 1,326 | 84,420 / 721,732 |
| Python | 4,705 / 31,652 | 2,192,232 / 17,730,488 |
| zlib | 284 / 691 | 95,784 / 469,392 |
| Bun | 34,070 / 43,849 | 15,847,616 / 32,231,712 |

Native-byte budgets exclude tables/stubs/alignment and are not RSS limits.
Ordinary dispatch weights are not execution/native-instruction coverage.

## Phase profiling and clock correlation

`tests/arm64/native-aot/phase-profile.ts` uses Linux perf user-cycle sampling
at 499Hz, `--clockid mono`. It freezes inputs, checks exact output/runtime
oracles and image diagnostics, resolves ELF file offsets through PT_LOAD
segments and AArch64 RELATIVE relocations, and classifies actual PCs inside
registered image text bounds. Named symbols identify image descriptors.

Python monotonic time and Bun's process-relative `hrtime.bigint()` cannot be
assumed to have the same origin as perf. A read-only, non-JIT-specific
`/proc/ish/profile-clock` reports host `CLOCK_MONOTONIC` nanoseconds as a
string. Four diagnostic snapshots bracket that read with runtime timestamps,
optionally also reading existing `/proc/ish/jit` counters. The profiler
intersects the possible offsets, checks snapshot/phase order, rejects an
incompatible constant offset and excludes uncertain phase boundaries.

Offsets assume a constant clock relationship over the run: small drift
within uncertainty or between observations is not disproved. Hot PCs are
phase-separated, with uncertain boundaries excluded. Snapshots execute guest
code; counter deltas include that instrumentation. These runs are **not**
unprofiled timing/RSS measurements.

Final diagnostic steady-state observations, one process per case:

| Runtime / variant | Samples | Native-image user-cycle share | Block lookup/translation requests | Installs | Busy-slot refusals |
|---|---:|---:|---:|---:|---:|
| Python full | 448 | 27.9% | 265,127 | 189,841 | 54,724 |
| Python selected | 436 | 16.7% | 185,586 | 66,211 | 19,230 |
| Bun full | 885 | 5.5% | 515,700 | 60,382 | 20,244 |
| Bun selected | 506 | 4.3% | 342,113 | 24,286 | 7,940 |

Native ranges include stubs, checkpoints and TLB guards, not just useful work.
These are sampled cycle shares, **not dispatch/instruction coverage**. Install
counts do not measure execution frequency, and request-minus-install is not
an exact eligible-image miss count. Context/lookup symbols, generator work,
gadget execution and dispatcher activity are retained in raw perf samples.
The initial Bun profile failed clock attribution and is retained as FAIL,
not silently aligned. Short warmup phases have too few samples for useful
percentages. TLB/guard cost is not independently quantified by this sampler;
TLB ownership/staleness is separately regression-tested, not inferred from
image acceptance or profile shares.

## Retained runtime improvement

`fiber_block_hash()` is shared by insertion, lookup and rehash. ARM64 uses
`((addr >> 2) ^ (addr >> PAGE_BITS)) % size`; other guests retain the previous
byte-address modulo. Complete address equality still decides matches.

Raw ARM64 addresses are four-byte aligned, so the former power-of-two table
used only a quarter of its buckets. Page folding also spreads equal in-page
instruction offsets. The profiler's hot dispatcher list walks motivated this
bounded change. Table growth policy, page hash, block lifetime, invalidation,
slot ownership, locking and publication are untouched. No stream/layout/ABI
change or dense AOT-context reindexing is involved.

Three alternating baseline/candidate pairs per runtime/backend at count64
(`hash-comparison/`, 36 executions) give these **median paired wall ratios**:

| Runtime / backend | Candidate / baseline wall change |
|---|---:|
| Python gadget | -11.2% |
| Python full AOT | -10.2% |
| Python selected AOT | -8.3% |
| Bun gadget | -11.5% |
| Bun full AOT | +4.5% |
| Bun selected AOT | -7.3% |

These are small exploratory samples, not confidence bounds or universal
speedups. Full Bun varied strongly between pairs. The first Python-gadget
baseline incurred 36 major faults; it remains in the raw data, not discarded.
Allocation/ASLR and workload behaviour can still vary measured RSS even when
the hash change itself allocates nothing and does not change cache capacity.
An independent bounded static review found no concrete defect in helper use,
full-address matching, bounds or rehash consistency; it is not proof.

### Rejected precise-page invalidation

The page hash conservatively invalidates all blocks in a bucket, including
unrelated guest pages. A candidate filtered by each block's actual endpoint.
An actual-core regression failed on baseline and passed candidate release,
debug and sanitizer lanes; diagnostic regeneration fell substantially.
However, retaining more blocks raised peak RSS and did not give an overall
wall-time win. For example, the separate count64 Bun-selected series rose
from 93,312 to 115,868KiB, with wall2.43→2.83s. These separate series are not
controlled paired speed estimates; they are sufficient to reject an RSS
optimisation claim. Candidate binaries, source/test patch and measurements
remain in `rejected-precise-invalidation/`, `timing-precise/`, `profile-precise/`.

The rejected filter does not solve shared-writable-alias coherence. Existing
invalidation is guest-virtual-page-local; collision eviction is not a reliable
alias-coherence mechanism. General backing-alias coherence remains a separate
pre-existing limitation. The retained hash change does not alter page buckets.

## Longer frozen evaluation

Final `timing-hash/`: counts1/16/64, one warmup unit, three alternating pairs
for gadget/full and gadget/selected. Each runtime/count has six gadget and
three samples of each AOT variant. There are **78 records**: six version
checks plus72 workload executions. Manifest SHA256:
`4ce0abc6af4130a911da377833ab053c9bb6dbf916544c9e1a7902c17f55efbc`.
All use the frozen first64 records, 42 active records, corpus SHA256 and exact
weighted-sum oracle 12,515,468 per unit. Internal phase times are monotonic;
GNU time records wall, peak RSS and faults. No native-version speed ratio.

Count64 medians (standard median, averaging the middle pair where even):

| Runtime / backend | Wall s | Peak RSS KiB | Steady s |
|---|---:|---:|---:|
| Python gadget | 2.245 | 23,936 | 0.868 |
| Python full | 2.800 | 51,068 | 0.863 |
| Python selected | 2.700 | 29,696 | 0.901 |
| Bun gadget | 2.025 | 70,804 | 1.022 |
| Bun full | 2.560 | 122,624 | 1.181 |
| Bun selected | 2.650 | 105,216 | 1.446 |

Selected images retain lower RSS than full images (~41.8% Python, ~14.2% Bun),
but both AOT variants remain larger/slower than gadgets at this count. Neither
runtime demonstrates a break-even across1/16/64; all four descriptive model
crossings are null. Do not extrapolate an acceleration. Startup residuals
include input/import/output and runner/accounting overhead, not causal loader
cost. Six major faults occurred in two final-series executions (5+1); the
rest were zero. There are no cold-disk claims.

Baseline and rejected-candidate complete series each have78 separate records.
The earlier externally interrupted baseline and failed clock/staging attempts
remain separate; they are never merged with completed series.

## Qualification

- **74 native/AOT/profile/selector/fusion/heldout tests, 472 assertions, no skips**
  with both ordinary and exact-profile gadget binaries supplied.
- Release/debug upstream correctness, actual-core 45-invocation policy matrix,
  and optimisation/concurrency tests. GCC ASan+UBSan actual-core policy/hash
  lanes use separately rebuilt instrumented archives, Clang assembly and
  libatomic; merely sanitizing the test wrapper would be insufficient.
- Block-hash tests: all initial buckets used for adjacent aligned PCs and
  consecutive equal-offset pages; maximum-address bounds; actual-core hash
  invalidation/teardown. Manual population does not independently test private
  production insertion/resize; real guest replay and source review complement it.
- Native emitter/exact restart and import fixes, including stale-TLB/CoW and
  lifecycle/cleanup checks. No ownership/reclamation change is inferred safe
  solely from a microbenchmark.
- Full and selected five-image no-emitter gates, zero rejected images,
  positive installs per image, zero emitted bytes and runtime-off parity.
- Ten independent actual hardware-PC hits: all five module text ranges in
  both full and selected debug no-emitter builds, NULL emitter region, exact
  output and normal exit. Image acceptance alone is not execution evidence.
- Linux sampler71 fixtures at O0/O2 and sanitizer/live probe; disposable
  delegated-cgroup bridge11 scenarios, ~97.1M peak, swap0, no OOM.

A first linked-gate invocation failed because its shell script is not executable;
explicit `bash` reruns passed. Review-service timeouts yielded no clearance;
the later completed source reviews and their limitations are retained separately.

## Reproduce and remaining boundaries

```sh
# Inputs/rootfs must already be staged and frozen; no building/training here.
bun tests/arm64/native-aot/phase-profile.ts /ABS/CONFIG /ABS/NEW_PROFILE 64
bun tests/arm64/native-aot/break-even.ts /ABS/CONFIG /ABS/NEW_TIMINGS
make test-arm64-aot-optim-contract
make test-arm64-block-hash CC=clang NINJA='ninja -j2'
```

The profiler optionally passes `--profile-jit` or `--profile-clock` to the
supplied Python/Bun workloads. Ordinary timing runs pass neither. The clock
endpoint is read-only and does not initialise or enable the native backend.

This host has no `xcodebuild`, Apple SDK/signing environment or attached Apple
hardware. Fresh Apple SDK builds, signed images, actual16KiB pages, app
lifecycle/backgrounding, jetsam and fault/recovery on devices remain **blocked,
not passed**. Linux synthetic geometry and native-PC tests cannot qualify them.
More native coverage/useful-work attribution, anonymous/dynamic-code fallback,
TLB/dispatch overhead and workload-specific image trade-offs remain research
areas, not an established on-device performance win.

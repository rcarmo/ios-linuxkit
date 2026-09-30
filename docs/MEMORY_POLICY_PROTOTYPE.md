# Experimental memory-pressure policy

Branch `prototype/memory-pressure-policy`, based on 2.3.2. This prototype is not
part of that release, and no app/CLI path enables or feeds it. The default
execution path and logical guest-page accounting remain unchanged.

## Scope

`kernel/memory_policy.h` defines an additional admission brake alongside the
existing hard `ANON_MMAP_LIMIT_PAGES` ceiling. It is implemented at
`anon_pages_reserve()` in `kernel/mmap.c`, preserving our explicit map ownership,
rollback and fork/CoW accounting. It does not import upstream's thread-local
precharge mechanism or host-page clustering.

- One short lock protects allowance, availability, critical-pressure flag,
  timestamp, enabled/valid state and hysteresis. Readers receive coherent fields.
- Disabled policy uses existing admission. Enabling/re-enabling requires a fresh
  sample and fails closed until one arrives.
- Critical pressure or headroom below 10% engages the brake; recovery requires
  at least 15%. The initial enabled state is braked and also requires 15%.
- Zero allowance/time, availability beyond allowance and out-of-order samples
  are rejected without replacing the last accepted sample.
- Admission rejects stale samples older than two seconds, timestamps in the
  future and requests larger than sampled available bytes divided by guest
  `PAGE_SIZE`. This is deliberately **not a host-footprint calculation**.
- The existing hard logical-page cap remains authoritative even with a feed.
  `PROT_NONE` remains uncharged; ordinary protection/commit paths retain their
  existing reserve/refund handling.

## Current evidence

Native Linux ARM64 on the Orange Pi 6 Plus, Debian Trixie, Clang 19, 4 KiB host
pages. Tests link the actual candidate `libish` archives. The extended
`tests/arm64/upstream/anon-accounting.c` covers missing samples, threshold
hysteresis, critical pressure, malformed and out-of-order samples, stale/future
admission, uncharged `PROT_NONE`, oversized requests, hard-ceiling preservation,
map-allocation rollback and 20,000 concurrent updates/reads. It retains the
existing mapping/protection/fork/CoW/lazy/mremap failure checks.

Run the real-kernel harness and compatibility suite through the existing gate:

```sh
make build-arm64-linux CC=clang RELEASE_BUILD_DIR=build-memory-policy-release
ISH_BIN="$PWD/build-memory-policy-release/ish" ROOTFS=/absolute/disposable-fakefs \
  EVIDENCE_DIR=/absolute/evidence CC=clang bash tests/arm64/upstream/run.sh
```

Successful output includes `memory-policy-actual-kernel-ok` and
`anon-accounting-actual-kernel-ok`. Release and debug actual-accounting harnesses
and the broader upstream correctness gate pass. The release build also passes
full procfs stress (native control plus two guest runs, each 25 seconds with
16 forkers and 6 readers). Local Markdown links and maintained-doc style checks
pass. Evidence is retained under `/workspace/artifacts/ish-2.3.2/20260930/` with
`memory-policy-*` names, separate from the release gates.

These tests do not run a host pressure sampler or validate Apple memory APIs.

## Not yet implemented

- No Apple physical-footprint/available-memory sampler or foreground/background
  hooks. No environment switch or app activation.
- No pending-commit budget between samples: individual requests can be admitted
  against the same availability. The policy is a brake, not strict enforcement
  of a sampled physical-memory budget. Do not enable it for production until
  reserve/refund/settle accounting and concurrency limits are defined.
- No special stack-growth/signal-recovery escape while braked. Test recovery
  before wiring app feeds; refusal must not become a host crash or guest loop.
- No 16 KiB allocation clustering, span accounting, Darwin protection tests,
  footprint measurements or jetsam/device proof.
- No AOT memory/performance claim. Held-out workloads remain separate future work.

The [upstream comparison](reports/audits/OPENMINIS_MEMORY_FOLLOWUP_2026-09-30.md)
records the remaining governor/clustering hazards and host/device gates. Keep the
prototype separate from `master` until those gates justify a bounded adoption.

# Experimental memory-pressure policy

Branch `prototype/memory-pressure-policy`, based on 2.3.2. This prototype is not
part of that release, and no app/CLI path enables or feeds it. Pressure admission
remains disabled by default. This branch also repairs CoW allocation failure and
checks admission before increasing anonymous-page protection. Failed signal-frame
installation now terminates the guest with SIGSEGV instead of recursively locking
the signal lock or retrying an unavailable stack.

## Scope

`kernel/memory_policy.h` defines an additional admission brake alongside the
existing hard `ANON_MMAP_LIMIT_PAGES` ceiling. It is implemented at
explicit admission-ticket APIs in `kernel/mmap.c`, preserving our map ownership,
rollback and fork/CoW accounting. It does not import upstream's thread-local
precharge mechanism or host-page clustering.

- One short lock protects allowance, availability, critical-pressure flag,
  timestamp, enabled/valid state and hysteresis. Readers receive coherent fields.
- Disabled policy uses existing admission. Enabling/re-enabling requires a fresh
  sample and fails closed until one arrives.
- Critical pressure or headroom below 10% engages the brake; recovery requires
  at least 15%. The initial enabled state is braked and also requires 15%.
- Zero allowance/time, availability beyond allowance, future timestamps,
  out-of-order samples and reused captures are rejected without replacing the
  last accepted sample.
- A sampler must call `ish_memory_policy_capture()` **before** measuring host
  availability. A publication settles only completions recorded in that capture,
  not transactions that finish during its measurement. Captures are single-use.
- Caller-owned tickets subtract outstanding allocation spans under the same
  lock. Actual map failure refunds the ticket and logical pages; successful
  allocation adds conservative debt until a later captured sample settles it.
  Unmap alone does not prove host memory reclamation and does not erase debt.
- Span cost rounds allocation bytes to `real_page_size`. CoW consumes a span
  ticket without charging an extra logical mapping. This is conservative host
  allocation cost, **not a measurement of physical footprint**.
- Missing/stale (over two seconds) feeds, critical pressure, insufficient
  remaining budget and the logical cap all refuse ordinary admission. Policy
  resets/toggles refuse while any transaction is outstanding.
- The existing hard logical-page cap remains authoritative in ordinary reserve
  paths even with a feed (fork's pre-existing bypass is noted below).
  `PROT_NONE` remains uncharged; protection/commit paths reserve before changes
  and refund failed admission. Range mprotect retains existing partial-change
  semantics; each successful page publishes a TLB generation even if a later
  page fails.

## Current evidence

Native Linux ARM64 on the Orange Pi 6 Plus: CIX P1 (CD8180/CD8160), 12 CPU
cores, 16 GB-class RAM (about 14 GiB visible), NVMe storage, Debian Trixie,
Clang 19 and 4 KiB host pages. Tests link the actual candidate `libish` archives. The extended
`tests/arm64/upstream/anon-accounting.c` covers missing samples, threshold
hysteresis, critical pressure, malformed and out-of-order samples, stale/future
admission, uncharged `PROT_NONE`, oversized requests, hard-ceiling preservation,
map-allocation rollback and 20,000 concurrent updates/reads. Twelve concurrent
claimants compete for four remaining spans; exactly four succeed and hold them
until refund. Fresh publication cannot erase pending claims; a capture made
before completion cannot settle that completion. Lazy and stack writes refuse
under critical pressure and recover after a fresh sample. Protection increase
and CoW/ptrace map/install failures refund their tickets and preserve old
flags/data; partial range-protection failure publishes earlier changes.

`cow-failure.c` also runs without the policy API against frozen `4e98cc8c`:
injected CoW `mmap` failure crashes the baseline (exit 139) and returns safely in
the candidate. Data-object allocation failure already fails safely in the
baseline and remains a passing control. The existing mapping, protection,
fork/CoW, lazy and mremap failure tests remain.

`signal-pressure.c` exercises actual `receive_signals()` and stack writes, wrapping
only final `do_exit_group()` to inspect the status and unlocked signal lock.
Critical-pressure refusal reaches guest SIGSEGV termination; a fresh recovery
sample permits frame installation. A configured SIGSEGV handler is deliberately
present in the refusal case. Frozen `4e98cc8c` uses the same fixture with only
policy API adaptation: refusal times out (exit 124), recovery passes. This is
kernel-path evidence, not an Apple app/device lifecycle test.

Run the real-kernel harness and compatibility suite through the existing gate:

```sh
make build-arm64-linux CC=clang RELEASE_BUILD_DIR=build-memory-policy-release
ISH_BIN="$PWD/build-memory-policy-release/ish" ROOTFS=/absolute/disposable-fakefs \
  EVIDENCE_DIR=/absolute/evidence CC=clang bash tests/arm64/upstream/run.sh
```

Successful output includes `memory-policy-actual-kernel-ok` and
`anon-accounting-actual-kernel-ok`. Release and debug actual-accounting harnesses
and the broader upstream correctness gate pass. Both release and debug also
pass full procfs stress (native control plus two guest runs per build, each
25 seconds with 16 forkers and 6 readers). Local Markdown links and maintained-doc
style checks pass. Initial prototype evidence remains under
`/workspace/artifacts/ish-2.3.2/20260930/` with `memory-policy-*` names; ticket and
failure/recovery evidence is under `/workspace/artifacts/ish-memory-admission-20260930/`.
All of it is separate from the release gates.

These tests do not run a host pressure sampler or validate Apple memory APIs.

## Not yet implemented

- No Apple physical-footprint/available-memory sampler or foreground/background
  hooks. No environment switch or app activation.
- No trusted platform capture/measurement adapter. The new ticket budget bounds
  simultaneous transactions in covered paths, not all host allocations or RSS.
  Page tables, file backing and unrelated host allocations are not budgeted.
  Fault injection covers backing-map/data-object failures, not page-table-node
  OOM, all lock-upgrade interleavings or ptrace reads of host `PROT_NONE` backing.
  Capture fields are an internal trusted API, not user-supplied authority.
- Fork charges shared logical mappings but does not reserve new physical pages;
  the existing fork path can exceed the logical ceiling. No claim of universally
  enforced process-footprint or guest-map budgeting is made.
- No special stack-growth/signal-recovery escape while braked. Ordinary writes
  refuse and retry after a fresh feed; failed signal-frame writes terminate the
  guest. There is no emergency allocator or guarantee of continued guest service.
- A [bounded clustering port](reports/audits/LOCAL_CLUSTER_PORT_2026-10-01.md)
  now exists on this prototype, disabled per address space by default. Actual
  ownership/failure tests use synthetic 4/16 KiB spans. No Darwin protection,
  footprint measurements or jetsam/device proof.
- No AOT memory/performance claim. Held-out workloads remain separate future work.

The [upstream comparison](reports/audits/OPENMINIS_MEMORY_FOLLOWUP_2026-09-30.md)
records the remaining governor/clustering hazards and host/device gates. Keep the
prototype separate from `master` until those gates justify a bounded adoption.

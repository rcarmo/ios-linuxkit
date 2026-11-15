# OpenMinis fixes, shared AOT paths and profiling — 15 November 2026

## Scope

This tranche follows `0912a50e` on `investigate/crate-openminis-20261115`.
It implements the applicable fixes identified in the
[Crate/OpenMinis audit](CRATE_OPENMINIS_2026-11-15.md), retaining the ordinary
no-JIT gadget interpreter, precise native restart, precharged memory tickets,
`anon_page_count`, existing user filesystems and strict image identity checks.
No Rust build, Crate integration, governor or clustering policy was imported.
Apple/device validation is deliberately outside this tranche's completion goal.

Host: Orange Pi 6 Plus, CIX P1 ARM64 12-core SoC, 16 GB-class RAM (about
14 GiB visible), NVMe storage, Debian Trixie, kernel `6.6.89-cix`, 4 KiB host
pages. Host-native runtime and workspace `/workspace` → `/home/agent/workspace`
were preserved. Toolchains: Clang 19.1.7 and GCC 14.2.0.

## Implemented changes and provenance

### Fd ownership and polling

Adapted the coherent lifetime work from OpenMinis `41e162c0`, socketpair
lock repair `e74295ed`, and orphaned-wait dependency `63ec0524`.

- `f_get_retained()` acquires a reference under the fd-table lock, not after
  a borrowed lookup. Epoll wait/ctl, poll and select balance it on success,
  errors, EFAULT and setup failure. Epoll event storage uses a checked heap
  allocation rather than a guest-sized stack array.
- Atomic ADD detection/insertion, locked cleanup/freelist mutation, and
  trylock/restart destruction preserve fd→poll lock order. Registrations do
  not retain member descriptions permanently. Retired backend-event storage
  is not reused while any waiter can still hold a pointer.
- Socket/pipe constructors consume host endpoints exactly once. Socketpair
  installation occurs outside `peer_lock`; original objects remain retained
  across publication. Accept no longer looks up under the inverted peer lock.
- Constructor rollback uses a pinned publishing table and per-slot generation
  token, not just the guest number or object pointer. Same-object close/rebind,
  unrelated replacement, changed current table and new CLOEXEC state survive
  stale rollback. Resize/copy generation allocation is transactional; generation
  exhaustion refuses reuse. Dup publications are locked and validated; dup2
  on the same valid fd preserves identity/CLOEXEC. Stdio publications are tagged.
- Group/sighand access at poll wait boundaries is protected by `pids_lock`
  with the poll lock dropped. Orphaned threads cannot resume writes through a
  freed group. Idle-exit requests defer SIGKILL until syscall cleanup rather
  than exiting while locks, retained fds or notify resources are held.
- Socketrestart listener cleanup balances the captured begin count even when
  members are removed/final-closed during a host wait.

Deadline work adapts `a57f8ce3` and `ae0e32bf` without replacing local
regular-file readiness/exclusion. One absolute monotonic deadline charges
rejected events, internal retries and backoff; zero/finite waits terminate.
Internal sockrestart interruptions at expiry return an ordinary timeout,
while genuine pending signals return EINTR. Spurious-wake backoff is unlocked,
at most 1 ms and bounded by the remaining deadline. Hangup-only kqueue
registrations add EV_CLEAR to contain wakes when NOTE_LOWAT is ignored.

Review-driven edge fixes add bounded nonblocking host harvesting before
zero-timeout readiness returns, preserving real ET drain/refill, duplicate
registrations, result capacity and oneshot rearm. Unchanged-union MOD avoids
synthetic duplicate host edges but explicitly wakes sleeping waiters through
the nonblocking notify pipe. These are correctness repairs, not measured
normal-path throughput claims.

### Shared PIC slow paths and profiling

Adapted `e6629fee` across the emitter, header stubs, relocations, ELF/Mach-O
export and both extra consumers (`kit.ts`, `record-apple.ts`). Three local
shared paths have validated stub IDs rather than invented host symbol names;
malformed/missing stub references fail closed. `ISH_JIT_STUBS=0` retains the
inline diagnostic lane. Precise snapshots, pinned/promoted registers, refused
links, poke/timer exits and no-emitter execution remain tested.

Completed `0820c6d6`'s producer/selector flow. `ISH_BLOCK_PROF_FILE` records
actual module paths and file offsets in both gadget-only and JIT-enabled
builds, merges load bases and flushes before CLI init shutdown bypasses atexit.
Disabled profiling avoids per-dispatch initialization, mapping lookup and locks.
`hotblocks.py` selects weighted coverage for existing `gen.py --keep`; omitted
translations continue through gadgets. This coverage is a heuristic, not timing.

Ordinary builds produce **dispatch samples**, since chaining bypasses the
observer. Exact counting requires a gadget-only build with
`DISABLE_BLOCK_CHAINING` and `DISABLE_RET_CACHE`. The former now consistently
compiles out eager incoming/outgoing prechaining, even if runtime knobs request
it: the original combination reproduced a host-pointer guest PC and exit 139.
The exact fixture now runs defaults and forced opt-ins, gives the expected
30 weighted guest instructions, and reports zero eager patches. Ordinary
fixture behaviour remains 11 sampled weighted instructions and 2/2 eager
outgoing/incoming patches. `make test-arm64-native-emitter` builds this exact
lane and runs it without a silently skipped fixture.

### TLB coherence and SHA512

Adapted the complete `21b420b3` protocol: MMU-attached TLB list/mutex lifecycle,
seq_cst generation/stale ordering, attach/detach around execution and orphan
cleanup, generated offsets and stale-bit inline gadget/native hits. Unattached
C lookups retain generation validation. Bounded translation retries return an
uncached result rather than relabelling stale entries. Remap/CoW/cache and
cleanup safeguards remain in place; this is not deletion of generation checks
without its lifecycle prerequisites.

Adapted `329056a6` host-feature detection and four software SHA512 gadget
fallbacks. Only `ISH_HOST_SHA512=0` forces software; other values cannot enable
unsupported hardware. Linux, Darwin and non-ARM detector fixtures, all operand
aliases, integer/vector register ABI, NZCV and FPSR preservation are tested,
including helpers that deliberately clobber caller-saved state.

Strict UBSan also found an existing signed-left-shift in two pair-offset
decoders. Multiplication now preserves all 640 intended scalar/SIMD offsets
without undefined behaviour. Poll idle traversal no longer forms an over-aligned
task pointer from a list sentinel.

## ABI and fresh Bun proof

Combined emitted-code/layout changes advance `JIT_CODE_VERSION` from 13 to 15.
Observed Linux PIC ABI: **`3babd760`**. TLB entries offset 40, stale offset 24,
inline stale displacement −16. ABI-13 recordings are historical evidence only;
no metadata was relabelled. This is not an observed Apple target contract.

Fresh Bun 1.4.4 five-image recordings:

| Image | Translations |
|---|---:|
| musl | 2,380 |
| BusyBox | 1,326 |
| Python | 31,652 |
| zlib | 691 |
| Bun | 43,850 |

All headers are non-exhausted. `build-arm64-bun15-aot` is built with
`jit_emit=false`; the final gate reports five images used, zero rejected,
zero runtime segments/units/code, no forbidden backend emitter imports and
BUN_TRAIN_OK/AOT_TRAIN_OK in both AOT and gadget modes. Original recorder identity
is retained even after later source fixes: compatibility is validated by the
new ABI and exact guest identities, not by pretending an old producer was new.

## Shared-stub image-density measurement

The initial comparison used different producer hashes and was explicitly
rejected as a strict comparison. Its report is retained, not rewritten. The
final series recovered the exact frozen inline producer and recorded only a
new shared counterpart using that same binary, module hashes, workload and
Bun no-JIT environment envelope. Both complete linked no-emitter gates passed.

Recorder SHA-256:
`37b555e43108fadb25227b4b74d185aefe6e4ec209dea880c64f4c780a9a5f30`.

| Metric | Inline | Shared |
|---|---:|---:|
| Linked image descriptor text bytes | 66,741,180 | 47,509,724 |
| Segment words per recorded unit | 48.3377 | 34.2939 |

Observed text reduction: **28.815%**; normalized words/unit reduction: **29.053%**.
Descriptor spans were cross-checked against generated assembly. Independently
trained coverage differs, so this is not an identical-translation causal
experiment or an application/device speedup. No paired timing claim is made.

## Reproduction and receipts

```sh
make test-arm64-import-fixes CC=clang NINJA='ninja -j2'
make test-arm64-native-emitter CC=clang NINJA='ninja -j2'
make build-arm64-linux-all CC=clang NINJA='ninja -j2'
make test-arm64-upstream CC=clang
make test-arm64-upstream CC=clang RELEASE_BUILD_DIR=build-arm64-linux-debug
make test-bun-rootfs test-aot-generator test-aot-kit test-xcode-gadget-guard
make test-rootfs-download test-go-rootfs check-docs
```

Passing qualification:
- Release/debug upstream suites; focused ownership/poll/SHA512 gates in both.
- Native O0/O2 preservation/restart/oracles, shared stubs and profiler fixtures.
  Exact and ordinary real guest profiling pass; no exact-profile skip.
- TLB lifecycle/unattached lookup/concurrent invalidation/cleanup/emitter and
  CoW probes at O0/O2; JIT-on/off guest tests.
- SHA512 helpers: 12,800 cases; gadget tuples: 131,072; guest decoder: 1,024
  instructions, forced/automatic/software/native modes as supported.
- Strict GCC ASan+UBSan: 11 selected production C units and 16 fixture builds/
  executions pass with zero diagnostics, leak detection and halt-on-error.
  Unrelated archive members, assembly and emitted machine code are not
  instrumented. Clang sanitizer archives are absent; GCC was used deliberately.
- Packaging/tooling/downloader fixtures and fresh linked no-emitter parity.
  One expected platform-specific Go packaging test remains skipped.

Evidence root: `/workspace/artifacts/ish-fixes-20261115`; final density report
`density/density-summary-final.{json,md}`, strict sanitizer report
`sanitizer-131026-strict-final/`, native `native-commit.log`, integrated
release/debug and linked-final logs. Artifacts are host evidence, not a
redistributable app bundle.

## Explicit exclusions

`b79f979c` fusion-bound prerequisites remain absent; a dormant flag was not
imported as an alleged improvement. No upstream memory governor/clustering,
broad borrowed-pointer socket rewrite or full-system Crate backend is included.
Darwin paths were inspected and Mach-O assembled where supported, not run as
a signed application. Broader preexisting fd/task lifetime issues are not
claimed comprehensively solved by these scoped fixes.

Actual Apple layout observation, final app membership/signing and physical-device
Bun/Pi/Go stability, lifecycle, footprint, sustained timing and thermals remain
unvalidated, deliberately not goals for today. Linux tests do not establish
that the reported device Bun/Pi or accelerated Mac Go crashes are fixed.

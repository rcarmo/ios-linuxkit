# OpenMinis imports — 28 September 2026

Baseline: ios-linuxkit `61719f81` (v2.1.3). Reviewed OpenMinis master `b08c12af`,
69 commits since the July-audited `89269e6f`. Ports are semantic where trees
have diverged; no wholesale branch merge, app-version change or experimental
native JIT/AOT/network import. This is a running import record, not a release.

## Tranche 1: reproduced correctness and filesystem/wait fixes

| Upstream commit | Imported change |
|---|---|
| `ace2cdbf` | Consistent scalar FMOV immediate bit6 convention; all 256 encodings per width. |
| `2427e6ea` | SIGEV_SIGNAL timer targets the group leader, not nonexistent PID zero. |
| `5d84ecd4` | Consume a selected pending signal before re-entering sigtimedwait. |
| `08eeb259`, `f8983200` | AArch64 open flag values and final-component O_NOFOLLOW resolution. |
| `565f4f82`, `2da30b04` | waitid status/code decoding and child UID capture before reap. |
| `19c690c3` | Release pids_lock and reset PID slot on task allocation failure. |
| `74d6a0df` | Synthesised archive root retains S_IFDIR. |
| `a4b5d7e3` (path subset) | Reject exhausted path output before slash/NUL overflow; existing base-path check retained. Fork hook assessed separately. |
| `359a268d` (adapted) | Our bind-mount stat branch gets host device/block metadata matching fstat; guest inode/ownership overlays retained. |

Source: https://github.com/OpenMinis/ish-arm64 (commit IDs above).

### Evidence

Host-native Orange Pi 6 Plus, CIX P1 (8 Cortex-A720 + 4 Cortex-A520), 16 GB-class
RAM, NVMe, Debian AArch64 Linux 6.6.89-cix, Clang 19.1.7; Debian fakefs. No native
offload. Evidence `/workspace/tmp/ish-import-20260928-aXyaSD/`; earlier baseline
failures `/workspace/tmp/openminis-check-20260928-XiXZr8/`.

- Unchanged baseline release/debug: FMOV 64/256 per width, 385 failed assertions;
  timers dropped; O_DIRECTORY/O_NOFOLLOW incorrectly succeeded; delayed selected
  signal timed out. Identical native probes passed.
- Candidate release/debug: FMOV 512/512; timer and signal probes pass;
  upstream syscall matrix **52 pass / 0 fail** on each build.
- `CC=clang make build-arm64-linux-all` passes after the ports.
- `tests/arm64/upstream/run.sh` compiles static native/guest fixtures, rejects
  skipped assertions, and requires exact result markers. Existing realfs path
  diagnostics are permitted; they are not interpreted as test success.
- Upstream tests are imported from `ace2cdbf` (FMOV) and `b08c12af` (syscalls).
  FMOV's erroneous prose "8x" corrected; instruction stream and oracle unchanged.
  Local bounded timer/flag/signal probes supplement the upstream matrix.
- Task-allocation failure and archive/path edges need dedicated fault-injection
  or targeted follow-up beyond this guest matrix; no such coverage is inferred.

## Boundaries

Preserve our native proc-mem seek semantics, FPCR/FPSR conversion gates,
full-width lseek, atomic poke and inline precise-PC improvements. Our crash
field offsets and CLI argument bounds already address overlapping upstream work.
Do not import the reverted poll/epoll changes as if they survived on master.

Lifecycle, OOM, sleep, accounting and filesystem imports are recorded below.
Memory/fork governor policy and Apple 16 KiB clustering are excluded pending
separate assessment; Linux evidence cannot establish iOS footprint behaviour.
No macOS build, signing, archive or device validation is claimed.

## Tranche 2: lifetime, timing and allocation safety

| Upstream commit | Adaptation |
|---|---|
| `80e444f1` | Unpublish mm/mem under general_lock, release outside it; proc maps/mem use the same lock. Preserve deferred-mm cleanup ownership. |
| `3bb6e27e`, `b311a744`, `a6c35797` | Interruptible deadline-loop sleeps with EINTR remainder; validate duration; destroy private mutex; honour every positive sleep (do not adopt upstream ≤50us yielding). Futex retains positive sub-ms remainder. |
| `84770265` | Deallocate the thread_info Mach send right in our platform/darwin.c rather than importing upstream's resource.c layout. |
| `f08572a0` | Fail gadget allocation without abort/finalising partial blocks. Distinct INT_OOM kills the affected guest thread group instead of retrying a mapped-page GPF. Release JIT locks on failure. |
| `f08572a0` (task subset) | Check positive pthread_create errors, propagate to clone/app, and explicitly unwind owned task resources. Local rollback also fixes fdtable_get using current's table rather than its argument. |
| `261bcd4e`, `a1e8b1e2` | Defer orphan cleanup only after namespace removal; retain inode during open; fstat outside global inode lock; handle rename replacement. |
| `3fa66c02` (transaction subset only) | Read transaction for noncreating opens. Do not cache exec stat snapshots or change creating-open transactions. |
| Local portability follow-up | Terminal and upgrade launch environments get the boot path's ARM64 PYTHONMALLOC setting. |

### Validation

- `CC=clang make build-arm64-linux-all` passes. Expanded
  `make test-arm64-upstream` passes against release and debug, including native
  oracles. Actual-archive injection tests cover **300 failed clones** (fork,
  vfork, shared-resource threads) and **100 failed app-task starts** per build,
  with resource-refcount/PID/list checks. Initial fixture setup errors (missing
  root mount/executable) were corrected before recording passes.
- Actual emitter injection covers initial allocation and growth failures, retained
  buffer/content and successful growth. Actual JIT entry injection checks frame
  and block OOM, both JIT locks released, and SIGKILL dispatch (termination itself
  is intercepted in the test). This caught a missing jetsam-lock release in the
  candidate and is now a regression gate. Not an all-host-allocations OOM claim.
- Bounded proc/exit: 30 rounds per run; open/unlink/rename-last-close: 50 rounds;
  concurrent open/unlink: **8/8 workers clean** native/release/debug.
- Precise 500us and 1.5ms futex waits and 200us nanosleeps pass native/release/debug;
  recorded candidate shortest 500us waits 519us/537us. Invalid, interrupted and
  blocked-signal nanosleeps pass. Extreme clock/range transitions remain outside
  these fixtures.
- Existing lseek-width, poke-stress, fcvt-vector, proc-mem-seek and
  load64-fault-PC gates pass on both builds. Load-PC coverage retains its isolated
  two-page-unmapped scope; no new PROT_NONE or second-page-only claim.
- Ten alternating baseline/candidate pairs pinned to CPU11, 10,000
  open/fstat/close operations each, same fakefs: baseline median **0.371973s**,
  candidate **0.320039s** (13.96% lower), candidate faster in **10/10** pairs.
  Baseline is saved v2.1.3; this is combined-tranche evidence, not per-patch
  attribution, and frequency was not fixed. Raw `fsbench-pairs.txt` and fixture
  are in the evidence directory. No general shell or iOS speedup claimed.
- The long upstream procfs shell stress previously timed out (124), never
  printed PROC_RACE_OK, and remains unresolved; bounded passes do not replace it.
- ASan link unavailable (missing Clang 19 AArch64 runtime libraries). Darwin/app
  changes are source-reviewed only; no Xcode, signing or device test here.

## Tranche 3: native-offload selection (`72dd6aaa`)

Adapted the exact-system-path and packed-environment policy as a header-only
implementation shared by Meson/Xcode. Generic ffmpeg/ffprobe offloads no longer
hijack relative/private paths, respect NO_OFFLOAD=1 and the upstream spelling,
and skip readable shebang wrappers. Synthetic Apple-only commands are unchanged.
The local shebang probe is nonblocking and limited to regular files, avoiding a
host stall when a selected path is a FIFO. Preserve argv/env cleanup handlers.

All **39** upstream policy assertions pass. A new actual-kernel sys_execve test
wraps only native-offload lookup/execution: it verifies a real filesystem
shebang wrapper is not offloaded, an absent system-path builtin is, and a private
path is not. Expanded release/debug gates pass. Linux's real registry remains
stubbed; this is not Darwin registry, host-spawn, or iOS runtime validation.

## Tranche 4: anonymous mapping ledger (`2d0094ad`, `a760a908`)

The actual kernel reproduced **count -3** after mapping/unmapping three
PROT_NONE pages (baseline assertion exit 134). Adapted the charge predicate and
all-path accounting to our existing 48-bit/lazy mappings, without importing the
footprint governor, device cap, host-page clusters or thread-local precharge.
Our private map helper takes an explicit precharged flag; reservation uses CAS,
transfers on success and rolls back on failure. Direct mappings, CoW and
protection transitions share the same predicate. Host mprotect failure leaves
old guest flags and charge intact. Existing cap value stays unchanged.

The actual-archive fixture (not upstream's standalone arithmetic model) passes
on release/debug: PROT_NONE teardown, internal maps, partial unmap, replacement,
protection transitions, fork/CoW, lazy fault, 1TiB virtual reservation, in-place
and moved mremap, cap rejection, and injected mmap/data-allocation/mprotect
failures. Expanded upstream gate and the five preservation gates pass on both.
This is a logical committed guest-page ledger, **not RSS/physical footprint**;
protection reduction can leave resident host backing. mprotect/fork accounting
is not a new hard cap policy, and fork can still take the ledger over its cap.

Internal-continuation preservation: 14/14 release and 14/14 debug. Initially
blocked by the absent Alpine lane, then the Debian lane's missing guest gcc;
the runner now accepts explicit HOST_CC=clang to build the same static fixture
on native AArch64 and stops immediately on setup failures. No skipped tests
counted as passes. Reports: `ish-arm64-internal-continue-fixtures-20260928-124529.md`
and `...-124530.md` in the evidence directory.

## Final source-audit follow-up

`bea892fc`/`57c1f142`: keep the debug pointer-equality invariant only across an
uninterrupted memory read-lock interval. Our lock-upgrade algorithm is retained;
mark all three local release/reacquire paths, including lazy reservations. This
is an assertion-scope correction, not a stale-pointer repair. Actual-kernel
CoW/lazy fixtures and debug poke stress pass; no new race reproducer is claimed.

## Complete disposition ledger

All **55 non-merge commits** in the reviewed 69-commit range are accounted for
below. Merge commits only connect these changes. “Deferred” is an explicit
exclusion from this import, not a tested fix waiting to be committed.

| Commit | Disposition and reason |
|---|---|
| `eb8f2b07` | Not applicable: local sync layer has no upstream slow-lock tracing overhead to compile out. |
| `74d6a0df` | Imported archive root S_IFDIR, tranche 1. |
| `a4b5d7e3` | Imported path bounds; host fork-guard policy deferred (no local host admission callback). |
| `123143f0` | Deferred cooperative in-process abort API: local subprocess forwarding already exists, but is not equivalent. Requires a real handler cancellation contract and Darwin tests; FakeFFmpeg does not supply one. |
| `359a268d` | Adapted bind-routed stat metadata, tranche 1. |
| `08eeb259` | Imported ARM64 open flags, tranche 1. |
| `4f6612d8` | Already present: internal wait timeout is retried; only actual EINTR marks signal interruption. |
| `f8983200` | Imported O_NOFOLLOW resolution, tranche 1. |
| `565f4f82` | Imported waitid status decoding, tranche 1. |
| `2da30b04` | Imported waitid child UID, tranche 1. |
| `b2f97ac6` | Already superseded by local FPCR/FPSR-aware FCVT vector implementation and gate. |
| `7e09bf0d` | Selected filesystem/wait matrix imported from final upstream tree; 52 assertions required. |
| `53739103` | Selected bounded scenario probes imported/adapted; not the 263-package suite or the unresolved procfs shell stress. |
| `19c690c3` | Imported task allocation lock rollback, tranche 1. |
| `bea892fc` | Adapted debug-only pointer invariant across local lock upgrades; no upstream locking algorithm copied. |
| `bf850b26` | Superseded by local /proc/mem full-width native seek implementation and gate. |
| `c77dd017` | Upstream seek commentary not copied over our deliberately different, documented native-seek semantics. |
| `57c1f142` | Adapted invariant commentary together with bea892fc. |
| `f7b0a737` | Already present: crash field offsets use offsetof; stale literal offsets not reintroduced. |
| `3f6384c7` | Deferred waitpid 1/2/4-second backoff: changes recovery latency for a performance aim; no measured many-waiter gain here. Existing 1-second timeout/retry retained. |
| `eee7f751` | Deferred fixed cap reduction: device policy, not portable correctness. |
| `54ab50a5` | Adapted central cap reservation/lazy-commit enforcement in tranche 4; device-derived cap and stack-window policy excluded. |
| `6320af44` | Not applicable without the host-byte/device-cap conversion; our ledger remains guest pages, not host footprint. |
| `ce1b4b86` | Deferred Apple host-page clustering; needs 16KiB host/device correctness and footprint evidence. |
| `1894e94e` | Deferred cluster/anonymous-mmap telemetry tied to that implementation. |
| `2d0094ad` | Adapted all-path mapping accounting and failure rollback, tranche 4. |
| `a760a908` | Adapted protection/fork charge predicate, tranche 4. |
| `6d1178f3` | Deferred footprint governor and hysteresis policy; Linux logical-page tests cannot validate jetsam behaviour. |
| `601891d4` | Deferred forced Node --single-generation: changes GC behaviour. Existing injected flags retained; local comment mentioning it is not evidence it is enabled. |
| `ace2cdbf` | Imported FMOV immediate correctness, tranche 1. |
| `f08572a0` | Adapted JIT OOM and failed thread-start handling with full local rollback and actual-archive injection, tranche 2. |
| `db38a54e` | Excluded: reverted upstream by 7856dcce; not a surviving fix. |
| `c36f7dfb` | Excluded: reverted upstream by 7856dcce; not a surviving fix. |
| `75f1e775` | Deferred CLI footprint-governor wiring, matching governor exclusion. |
| `7856dcce` | Respect revert by importing neither poll/epoll candidate. |
| `5d84ecd4` | Imported signal consumption, tranche 1. |
| `097492fe` | Deferred iOS footprint-governor wiring; no local device validation. |
| `2d560913` | Excluded app release/deployment metadata; keep v2.1.3/build809 unchanged. |
| `84770265` | Adapted Mach send-right release to platform/darwin.c, tranche 2. |
| `e5d82e6c` | Not needed: diagnostic hook interface was not imported; local platform layer builds without it. |
| `2427e6ea` | Imported POSIX timer delivery, tranche 1. |
| `3bb6e27e` | Adapted interruptible sleeps, tranche 2. |
| `b311a744` | Adapted recomputed deadline/remainder loop, tranche 2. |
| `713e594c` | Excluded unconditional high-volume mm-release diagnostics; not a correctness fix. |
| `efc8a1ac` | Deferred mm-sequence diagnostic plumbing; not required by retained fixes. |
| `42ef4fbe` | Excluded shared normalized-path cache: key is path+flags, omits cwd/root/credentials; hit bypasses permission/path traversal. Generation invalidation does not make those contexts equivalent. Our local path walker has no such cache. |
| `261bcd4e` | Adapted deferred orphan lifecycle and rename replacement, tranche 2; repeated combined filesystem gain measured. |
| `a1e8b1e2` | Adapted early inode retention/unlocked fstat, tranche 2; concurrent open/unlink passes. |
| `0e843724` | Deferred trace-gating interface: diagnostics remain noisy, but no separate measured benefit or required host API. |
| `80e444f1` | Adapted mm unpublication/proc reader locking, tranche 2. |
| `2d128d7a` | Excluded path-cache/fork counters for upstream CPUTop sampler; absent local consumer/cache. |
| `a6c35797` | Adapted precise positive sleeps/futex deadlines; upstream short-sleep yield policy rejected. |
| `3fa66c02` | Only read transaction for noncreating opens retained; cached stat at exec rejected because permission/ownership/size metadata can change between operations. |
| `e42dccac` | Deferred fork-rate token bucket: admission/performance policy, not required for fork correctness; no measured local workload benefit. |
| `72dd6aaa` | Adapted generic offload path/env/script safeguards, tranche 3. |

Experimental branches (AOT, HyperJIT/native JIT, networking) were not merged.
No broad runtime/package pass, ASan pass, long-proc-stress pass, or iOS device
pass is implied. Remaining known limitations include broad-suite Debian package
detection, clone3 alternate-stack coverage and the native LDXP/STLXP SIGBUS
fixture noted in the July audit. The existing task teardown safety-valve design
is preserved, not certified race-free by these bounded tests.

## Reproduction and publication

```sh
CC=clang make build-arm64-linux-all
for build in build-arm64-linux build-arm64-linux-debug; do
    CC=clang make RELEASE_BUILD_DIR="$build" test-arm64-upstream \
        test-arm64-lseek-width test-arm64-poke-stress test-arm64-fcvt-vector \
        test-arm64-proc-mem-seek test-arm64-load64-fault-pc
    HOST_CC=clang make RELEASE_BUILD_DIR="$build" \
        ROOTFS_DIR="$PWD/debian-arm64-fakefs" test-arm64-internal-continue-fixtures
done
make check-docs
git diff --check
```

Final closure repeats all six focused gates on release/debug, plus 14/14
internal-continuation checks per build. Supplemental CAS128, CLREX/STXR,
exclusive-width and LDPSW fixtures match native output exactly on both builds.
`make check-docs` checks 44 Markdown files; diff checks pass. Binary hashes and
closure logs are retained in the evidence directory.

Set EVIDENCE_DIR to retain upstream-gate logs/binaries; otherwise temporary files
are removed. Imports are local reviewable commits, no version bump or tag.
No push of this import series has been performed.

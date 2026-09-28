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

Further lifecycle, OOM, sleep, accounting and filesystem improvements remain
under review. Memory/fork governor policy and Apple 16 KiB clustering need
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

# Native/AOT integration staging — 29 September 2026

## Scope and references

User authorised shipping the current engine first, then selective native/AOT
integration, targeted images and an Alpine upgrade. Current-engine source release
2.2.2/build812 is published at `5ff9f426`, tag `v2.2.2`.

This branch, **`integrate/native-aot-alpine`**, starts from that release. It is not
a wholesale merge of the upstream-derived branch. Backend donor remains
`b50541ee` (`fix/jit-aot-safety`, upstream PR
[OpenMinis/ish-arm64#45](https://github.com/OpenMinis/ish-arm64/pull/45)).
**No native/AOT source or enablement has yet been imported into this branch.**
No new version, tag, iOS binary, speedup or completed integration is claimed.

## Alpine preparation and package versions

The app previously pinned Alpine 3.24.0; this checkout had only a Debian fakefs
validation lane. Fetched the official stable aarch64 release metadata and pinned
**Alpine minirootfs 3.24.2**, dated 17 September 2026:

- URL: `https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.2-aarch64.tar.gz`
- SHA-256: `9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773`
- Archive size: 4,028,030 bytes.

Updated the three rootfs settings (`GuestARM64`, `Linux`, `iSH`) on this branch
only. `download-root.sh` now requires the checksum, stages the download next to
the destination, verifies hash and ELF architecture, then atomically replaces
the bundle archive. Fetch/hash/architecture failure preserves an existing good
bundle. Offline tests exercise the real shell script with only curl mocked;
a real network fetch also matched the pinned bytes.

An isolated fakefs was imported with `fakefsify`. After supplying resolver
configuration through guest tar (never backing-file writes), guest `apk update`,
`apk upgrade` and `apk add python3 zlib` succeeded. Observed installed targets:

| Component | Version |
|---|---|
| apk-tools | 3.0.8-r0 |
| musl | 1.2.6-r2 |
| BusyBox | 1.37.0-r31 |
| Python | 3.14.7-r1 |
| zlib | 1.3.2-r0 |

The isolated validation rootfs has 33 packages, approximately 50.4 MiB according
to apk. Python JSON/hashlib/zlib round-trip smoke passes. These added packages
are **not** injected into the app's stock minirootfs by this change, and existing
installed app userlands are not migrated automatically.

Initial package-query warnings were missing guest DNS, not package success.
A first gate invocation used an absolute Make build path in a recipe that prefixes
CURDIR; that setup failure was corrected with a relative path before passes.
A delegated read-only inventory timed out and is not counted as evidence.

## Compatibility findings before backend work

### Regular-file poll/select and BusyBox ash

Alpine proc stress initially timed out after workers had exited: BusyBox ash's
`read` polls a regular counter file before reading it. Host epoll rejects regular
files/directories with EPERM; `poll_add_fd` removed the registration and the
caller proceeded to wait with no file to observe. `echo 123 >file; read n <file`
reproduces the hang independently. Captured stack ends in sys_ppoll/poll_wait.

The fix leaves regular files/directories in the userspace readiness scan rather
than registering them with host epoll. Their readiness is IN|OUT independent of
access mode/EOF, matching Linux. Guest epoll_ctl still rejects them explicitly.
A second native-oracle check exposed pselect6's optional NULL sigmask-argument
pointer being unconditionally dereferenced; initialise the local argument to
zero and read it only when present.

`poll-regular.c` passes natively and on candidate release/debug: access modes,
EOF, dup, directories, poll/ppoll/select/pselect, mixed pipe readiness and epoll
file rejection/pipe acceptance. Original 2.2.2 fails the first poll assertion
(guest assertion produces CLI status1, not host abort134). The actual BusyBox
shell read now completes. This is not a wholesale import of the earlier reverted
OpenMinis epoll patches, nor a complete poll syscall audit.

### Normal-exit task TLS lifetime

After the read blocker was fixed, Alpine debug stress crashed with host SIGSEGV.
A repeat with cores enabled captured `task_run_tlb_cleanup -> mm_release(0x1b00)`.
`current` pointed to reclaimed/reused task storage: normal group-leader do_exit
left TLS intact after unlocking pids_lock, allowing parent reap before pthread
cleanup inspected deferred-mm fields.

Normal exit already released its mm. It now clears `current` for leaders as well
as non-leaders **before releasing pids_lock**. Deferred/safety-valve paths are not
claimed repaired by this bounded change. `exit-current.c` wraps the real
pthread_exit handoff, deterministically asserts NULL TLS and reaps 100 actual
kernel-created child tasks. Baseline actual archives abort134; candidates pass.
This gives a cause for this captured crash, not proof about the earlier unrelated
September proc-stress exit139.

## Validation and unresolved evidence

Host: Orange Pi 6 Plus, CIX P1 12-core AArch64 (8 Cortex-A720 + 4 Cortex-A520),
16GB-class RAM/~14GiB visible, NVMe, Debian Trixie, Linux6.6.89-cix, Clang19.1.7.
Evidence: `/workspace/tmp/ish-aot-integration-hmvkF9/`, pointer
`/workspace/tmp/ish-aot-integration-path`. Original core is retained there.

Candidate release and debug builds pass on **both Alpine3.24.2 and the existing
Debian rootfs**:

- expanded upstream gate, including new 100-exit handoff regression;
- regular-file readiness native oracle and actual shell read;
- lseek, poke, FCVT, proc-mem seek and precise load-PC gates;
- 14/14 continuation fixtures;
- full proc gate with native + two guest 25s runs, 16 forkers/6 proc+ps readers,
  every worker making progress and terminating cleanly.

After the exit fix, additional direct full Alpine stress runs passed: three
release and three debug, plus one traced debug run. **One earlier post-fix release
run timed out124 with an empty log and no captured stack. This remains an
unattributed intermittent failure at the first checkpoint; later passes did not
erase it.** It was subsequently reproduced and diagnosed below. Darwin/iOS and
sanitizer validation remain unavailable.

### Retained timeout diagnosed: proc-stat / signal-frame lock inversion

Full-load run14 hung again; run15 captured a core and lock ownership. Proc-stat
held pids_lock and the target group lock, waiting for its sighand lock. That
target was delivering a signal: sighand held → user_write_task → mem_ptr stack
growth → rlimit → group lock. The two owners and mutex addresses establish the
ABBA cycle; this is distinct from the earlier inode/mm and stale-TLS defects.

`proc_pid_stat_show` now snapshots pending/blocked/ignored/caught fields under
sighand separately, before taking general/group locks. pids_lock still protects
task/sighand lifetime. No lock is simply omitted; the report is a snapshot, not
an atomic view across unrelated subsystems. Existing signal-field encoding is
preserved. The actual-archive `proc-stat-locks.c` wraps pthread_mutex_lock and
requires group/general to be available when the callback takes sighand. Baseline
aborts134 (both EBUSY16); fixed release/debug pass and preserve the signal fields.

Validation after this fix: four full Alpine guest stress runs per build (eight
in total), native controls, and two Debian guest stress runs per build all pass
with unchanged25s/16forker/6reader denominators. Both builds also pass expanded
upstream, poll/read, five preservation gates and14/14 continuation. Evidence is
`timeout-investigation/` beneath the evidence directory, including `hang-14.log`,
`hang-15.core`, raw lock-owner diagnostics and the deterministic negative test.
These bounded tests address the captured cycle, not every possible concurrency
fault. Backend integration and platform acceptance remain separate gates.

## Next selective integration steps

1. Import repaired `jit.c`/`jit.h`/`aot.h` and only their required block-lifecycle,
   emitter-unit, native-link, proc-control, build and recovery hooks. Preserve
   local 48-bit/lazy memory, precise PCs, FP state, OOM and continuation behavior.
2. Keep native/AOT default off. Production donor requires Apple arm64; Linux
   adapters are tests, not a validated platform port. A Darwin recording/signing
   environment is required for the intended full pipeline.
3. Prepare only musl, BusyBox, Python and zlib initially, against this actual
   rootfs. Upstream manifest says `libpython3.12` and extension `cpython-312`;
   replace assumptions with inspected Python3.14 module paths/build IDs.
4. Harden recording: upstream `record_images.sh` tolerates failed workloads,
   pipes generator output without preserving its status and uses bare wait for
   multiple children. Do not count images from that unchanged runner as verified.
5. Fresh-process gadget/AOT-only comparisons, exact output checks and executed
   native coverage before performance claims. Old ABI7 images cannot be reused
   after checkpoint changes; integration layouts will determine the new ABI hash.
6. Retain full stress plus the deterministic proc-stat lock-order test throughout
   integration; the formerly unattributed timeout is now diagnosed above.

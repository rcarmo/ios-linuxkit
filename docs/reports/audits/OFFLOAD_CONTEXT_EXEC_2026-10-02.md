# Cooperative guest-context execution — 2 October 2026

## Scope

Isolated `harden/cooperative-offload`, based on v2.4.0/build817 and filesystem
foundation `fdf39cf4`. No memory-policy/clustering code is imported. Mainline,
release version, app targets, AOT images and default gadget configuration remain
unchanged. This tranche completes the explicit filesystem execution wiring;
it does not complete bounded streams, cancellation or a production handler.
No cooperative handler is registered by the app or CLI.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four
Cortex-A520 cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, host-native
Debian Trixie AArch64, 4 KiB host pages, Clang 19.1.7. No Apple SDK/device proof.

## Execution contract

`native_offload_add_cooperative_handler` registers a separate callback receiving
raw guest argc/argv and a borrowed `native_handler_context`. The context exposes
`native_handler_fs`, returning the retained guest root/CWD/umask snapshot only
on the owning guest task and pthread. It does not expose host paths or borrowed
host descriptors. Callers open through `native_fs_open`, use the guest VFS and
close owned descriptors before return. Workers must not retain the context.

The cooperative dispatcher is separate from legacy handler/spawn execution:

- no process-wide chdir/fchdir, host argv rewriting or backing-path translation;
- no host directory scans or post-hoc fakefs registration;
- no forwarding pipes/threads, hidden stdio borrow or stream API yet;
- checked argv/context allocations before exec commitment, exact unwind on error;
- retained snapshot ignored by later task CWD changes;
- single-thread guest group required, and group-exit state refused with EBUSY;
- after setup, existing comm/CLOEXEC/signal/altstack exec commitment applies;
- after callback return, context/argv are released before masked exit status.

Registry writes are startup-only. `task_start` freezes the registry before
pthread creation; `task_run_current` covers direct CLI execution too. A failed
first launch does not reopen the registration window. All registration methods
reject after the freeze. Startup registration itself is intentionally
single-threaded; this is not an API for concurrent registration/freeze calls.
The documented legacy host-path/stdio execution behaviour is unchanged.

This is a scaffold, not forced or cooperative cancellation. No token is
published, no deadline is asserted for VFS opens/disk I/O, and no real FFmpeg or
network operation is admitted. Full guest signal/exec semantics are not claimed.

## Focused verification

```sh
CC=clang EVIDENCE_DIR=/absolute/fresh-evidence make test-arm64-offload-context \
  RELEASE_BUILD_DIR=build-offload-release
```

`offload-context.c` links actual candidate archives and the portable dispatcher
through a Linux-only adapter. Both realfs and imported fakefs roots prove:

- unchanged absolute/relative/URL arguments, mounted-file selection, relative
  and absolute symlink behaviour, O_NOFOLLOW refusal;
- output immediately readable through VFS and visible in guest metadata, with
  captured umask 0027 yielding mode 0640;
- concurrent execution on independent guest tasks, in different CWDs, with
  synchronised reads/creation and rejection of another task's context;
- seven deterministic argv/context allocation failures, tracked allocation
  balance, unchanged guest comm/did_exec/signal/altstack, root/CWD reference
  counts and host descriptor count;
- missing-CWD refusal, sibling/group-exit refusal before guest commitment;
- registration allocation failure and registry freeze via an injected first
  pthread-start failure;
- host CWD unchanged and no host descriptor leak after normal cleanup.

The fixture aborts on chdir, fchdir, opendir or pipe calls, including legacy
post-hoc registration. This detects accidental use of those paths rather than
merely checking final CWD restoration. Callback negative status is masked without
undefined negative shifts. Compiled with -Wall/-Wextra/-Werror.

## Broader verification

Evidence: `/workspace/artifacts/ish-offload-context-wiring-20261002/`.
Four lanes: gadget release/debug and compile-time no-emitter AOT release/debug.
Every `gates-<lane>/{build,context,fs,setup,upstream,proc}.rc` is 0. Both
`gates-aot-<mode>/linked.rc` are 0: four frozen modules used, runtime-off parity,
no emission, no retraining/ABI relabelling. Fresh per-lane roots restored from
the v2.4.0 safety seed; proc stress runs native plus two guest iterations, each
25 seconds, 16 forkers/six readers, with required progress and worker shutdown.

After strengthening only the fixture with allocation tracking, foreign-context
rejection and actual first-launch freeze, final context suites pass in all four
`final-<lane>/context.rc`. The unchanged kernel hashes match the earlier broad
runs; `final-sources.sha256` captures the final fixture/source set. All four
`final-<lane>/compat.rc` are 0, covering FCVT, precise load PC, proc-memory seeks,
full-width seeks, poll/readiness, internal continuation and scalar saturation
(19,696 native/guest cases). Compatibility starts only after preceding proc and
linked runs complete, avoiding shared-root concurrent mutations.

The first fixture compile failed for a missing host fcntl header and a signed
size comparison. These errors remain in `release-first.log/.rc` (2), not counted
as passes. Corrected and final reruns pass. The read-only I/O delegate timed out
120 seconds; there is no independent-review claim. Documentation/link/style and
diff checks pass in `docs-tools.rc` (0). Xcode gadget guard and AOT generator/kit
tool suites also pass. A last fixture-only strengthening checks a valid legacy
callback is refused after registry freeze; all four `publish-<lane>.rc` are 0,
and `publication-sources.sha256` captures this final source set.

## Next bounded-I/O design constraints

Source inspection confirms why a poll-then-write wrapper is insufficient:

- `fs/tty.c:tty_poll` advertises POLL_WRITE unconditionally, including PTYs;
- `tty_write` reads shared fd O_NONBLOCK, but acquiring tty locks can wait and
  its driver contract does not promise bounded host operations;
- `fs/tty-real.c:real_tty_write` ignores the blocking argument and uses host
  write on stdout;
- `app/Terminal.m:ios_tty_write` ignores the blocking argument, and `sendOutput`
  waits on dataConsumed with signals ignored when the UI output buffer is full;
- `fs/pty.c:pty_write` enters tty_input, which can wait for consumer space and
  has additional line-discipline/echo/poll/signal lock interactions;
- realfs read/write can block; duplicating a descriptor does not isolate its
  shared open-file-description O_NONBLOCK setting.

The next tranche needs explicitly admitted, per-call nonblocking backend
operations, bounded chunks/budgets, token checks during retry/backpressure and
ownership-preserving shutdown. Unsupported backends must fail before exec
commitment; never detach or free a live task/token-using worker after a timeout.
Socket MSG_DONTWAIT/NO-SIGPIPE and restricted in-memory endpoints are possible
initial adapters; arbitrary TTY/file callbacks are not automatically safe.
The Apple UI sink needs its own bounded admission operation and target tests.
Linux proof does not establish Apple 16 KiB protection, app lifecycle, signing,
footprint/jetsam or physical-device safety.

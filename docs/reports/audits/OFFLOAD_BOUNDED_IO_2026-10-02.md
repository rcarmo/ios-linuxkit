# Cooperative bounded streams and token isolation — 2 October 2026

## Scope

Isolated `harden/cooperative-offload`, based on v2.4.0/build817 and context
wiring `861526f2`. No memory-policy, clustering, native emitter, app scheme or
production handler is enabled. Existing legacy handler/spawn I/O is not made
bounded by this tranche. The validated default-off tranche was subsequently merged as recorded below.

Host: Orange Pi 6 Plus/CIX P1 (CD8180/CD8160), eight Cortex-A720 and four
Cortex-A520 cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, host-native
Debian Trixie AArch64, 4 KiB pages; Clang 19.1.7. Apple SDK, signing, app terminal
and physical-device gates have not run. This is Linux host proof only.

## Stream admission and operations

`kernel/native_io.c` introduces guest-thread-only read/write/check/signal APIs.
Stdio references are retained under the guest fdtable lock, excluding CLOEXEC,
and individually released after callback cleanup. Admission precedes exec
commitment and verifies actual guest socket fdops, guest INET/INET6 stream type,
host SO_TYPE and connected peer family. Only connected TCP streams are admitted.
Regular files, pipes, TTY/custom fdops, Unix sockets, datagrams, unconnected or
mismatched sockets are refused before exec commitment. Missing/CLOEXEC streams
remain closed; attempts to use them produce EBADF, never inherited host stdio.

Unix sockets are intentionally excluded: raw recv would bypass guest SCM and
peer bookkeeping. Arbitrary fd callbacks are excluded even when poll reports
ready. Nothing sets O_NONBLOCK or other open-description flags. Output uses
MSG_NOSIGNAL where available; Darwin must already have SO_NOSIGPIPE set, checked
without changing a shared socket. Darwin output without safe SIGPIPE suppression
is refused. This branch has not been Apple-compiled or device-tested.

Per-call send/recv uses MSG_DONTWAIT, at most 4 KiB, and no hidden retry. A retry
loop observes cancellation, sticky errors, monotonic execution and per-call
deadlines before each retry, sleeping in at most 1 ms slices. Wait budget is at
most 250 ms per call. Zero-wait EAGAIN is nonfatal; short transfers are reported
exactly and must be handled by the callback. Read EOF returns zero. Zero-progress
write and backend errors are fatal/sticky. Combined stdout/stderr has a 1 MiB
budget, separate from the 1 MiB input budget. Over-budget requests fail before
transfer. Invalid arguments/foreign contexts fail without state changes.

Execution checkpoints and streams have a 5-second monotonic deadline. The
executor also checks after callback return. A sticky I/O/budget/deadline error
forces nonzero exit if the handler returns success; cancellation signal status
wins. This is not a watchdog or a hard real-time guarantee: it cannot preempt a
callback that does not poll/return, and scheduler/kernel delays are not bounded
by user-space code. Filesystem opens/disk I/O/cleanup have no universal wall-clock
bound. The API is a restricted cooperative contract, not forced cancellation.

No pipe-forwarding workers or task/token-using helper threads are created.
Callback-owned workers must be joined before return, and must not use the
thread-owned context. The framework never detaches a live worker or frees a
stack token while a sender may still access it. No pthread_cancel is used.

## Cancellation lifecycle

Only `native_cancel.c` and narrow task/signal integration are extracted from the
combined prototype; its legacy chdir/blocking executor is not imported.

The token is published and withdrawn under sighand.lock. Eligible INT/TERM/HUP/
QUIT requests are sticky; KILL supersedes them even if blocked. Blocked/ignored
eligible signals do not cancel. Other guest signals retain their existing queue
semantics; this is not a complete in-process guest signal handler/STOP model.
Already-pending eligible signals are mirrored at publication and skip the
callback. Publication precedes exec signal-action reset. The ordinary queuing
path checks again under the same lock to close an early-miss/late-queue race.

The token stays published through callback resource cleanup and stdio/context/
argv release. Completion withdraws it under the signal lock; requests after
withdrawal go to the normal queue and cannot dereference the stack token.
Completion and cancellation have an explicit linearisation boundary, not a
promise to incorporate every signal arriving after completion. Task construction
clears native token/proxy/pid state after copying a parent, isolating forks.
Single-thread/not-exiting guest-group admission and frozen startup registration
remain required. Existing ordinary guest execution has no token.

## Focused tests

```sh
CC=clang EVIDENCE_DIR=/absolute/new-evidence make test-arm64-offload-io \
  RELEASE_BUILD_DIR=build-offload-release
```

`offload-io.c` compiles the real portable dispatcher with a Linux test adapter,
links actual kernel archives and uses real loopback TCP send/recv. Thirty-nine
independent dispatcher modes cover:

- ordinary stdin/stdout/stderr, partial writes, EINTR reads/writes and EOF;
- actual full TCP send buffers, bounded timeout and a joined host-only consumer
  that drains backpressure; exact full 1 MiB output budget including short writes;
- cancellation while waiting on empty input/full output, joined signal sender;
- malformed requests and foreign-thread context/read/write/check rejection;
- sticky zero/error/broken-send/read errors even when callback returns success;
- host SIGPIPE set to DEFAULT, demonstrating MSG_NOSIGNAL suppression;
- separate input/combined output budgets and execution deadline/return checks;
- simulated scheduler overshoot proving no extra syscall after retry deadline;
- cancellation injected during context cleanup, token still published;
- closed/CLOEXEC stdio, retained alias balance and unchanged host flags/fd count;
- unsupported regular file/pipe/custom/Unix/unconnected/type/family refusal;
- every new admission syscall failure and precommit clock failure rollback;
- nested-token refusal; prepublication TERM/KILL skip; blocked/ignored controls.

The fixture uses private context fields only to observe budgets or accelerate
deadline/error cases. This is not a public setter or an ABI diagnostic override.
Actual TCP prefill/drain and transfer operations are not mocked. Send/recv wrappers
check MSG_DONTWAIT/NOSIGNAL, chunk size and unchanged shared descriptor flags.

`offload-cancel.c` runs actual task construction and signal code: pending-before-
start, blocked/ignored, KILL priority, fork token/proxy isolation, explicit
publication-between-early-request-and-queue, plus 1,000 real signal/completion
races. Every thread is joined, token withdrawn and pending queue cleaned. Counts
before/after completion vary with scheduling; their sum must be exactly 1,000.

The existing realfs/fakefs context fixture still proves seven allocation rollback
slots, parallel guest CWD/metadata/mount semantics and no host chdir/scans/pipes.
Existing legacy handler/spawn setup fixtures are rerun unchanged.

## Evidence and limits

Evidence root: `/workspace/artifacts/ish-offload-bounded-io-20261002/`.
Final source/fixture hashes: `gates-sources.sha256`. Four lanes are gadget
release/debug and compile-time no-emitter AOT release/debug, with fresh independent
v2.4.0 safety-seed roots. Every `gates-<lane>/{build,io,context,fs,setup,upstream,proc,compat}.rc` is 0;
both `gates-aot-<mode>/linked.rc` are 0. The runner completed at 07:52:28 UTC
before status collection. All source hashes still match. Proc stress requires
native plus two guest runs, each 25 seconds, 16 forkers/six readers, progress and
worker shutdown. Compatibility includes FCVT/load-PC/proc-memory/full-width
seek/poll/internal continuation/scalar saturation (19,696 cases). Both linked
gates use four frozen images, runtime-off parity and zero emission; no images
are retrained or ABI recordings relabelled. `docs-tools.rc` is 0 for links/style,
style fixtures, Xcode gadget guard and AOT generator/kit. Diff checks are clean.

The first build command used NINJA='ninja -j4'; Meson regeneration expects an
executable name and failed detection. Retried with NINJA unset and built
successfully. This runner error is retained as build-first.rc (2), not a pass.
The strengthened full-budget fixture initially requested a whole 4 KiB after a
short transfer left less than a full chunk; the candidate correctly refused the
over-budget request, exposing the fixture assertion. Corrected request sizes
reach exactly 1 MiB and refuse the next byte. io-strengthened.rc (2) is retained;
io-fixed-budget.rc is 0. A delegated narrow review timed out at 90 seconds;
there is no independent-review claim.

## Mainline integration

Published branch commit `6c2bd5fc`, then merged the clean v2.4.0-based hardening
branch into master at `7ccc3c92`. Its tree is byte-identical to the four-lane-tested
candidate, with no combined prototype history, handler registration, app AOT
activation or release/version change. New mainline Clang release/debug directories
build successfully, and both `mainline-<mode>-{io,context}.rc` are 0. These include
another 1,000 cancellation races per mode and realfs/fakefs concurrent execution.
The merge remains a source change, not a signed Apple archive or source release.

The first fresh mainline builds omitted explicit CC=clang. GNU Make's built-in
CC=cc overrides the Makefile's CC ?= clang; GCC's assembler rejects the existing
ARM64 gadget register aliases. Both `mainline-<mode>-build.rc` (2) are retained,
not counted as passes. Fresh `build-offload-mainline-clang-<mode>` directories
created with CC=clang pass. No compiler override is hidden in the old directory.

Further work: bounded local-file handler and explicit partial-output policy;
a separately tested Apple
UI nonblocking admission sink. Existing app Terminal sendOutput waits for UI
buffer space ignoring signals and must never be admitted through this API as-is.
Regular-file/VFS operations are not magically bounded by the stream deadline.
Apple scheme/target ABI/Mach-O membership/signing/device/rebaseline gates remain
separate and mandatory before app AOT enablement.

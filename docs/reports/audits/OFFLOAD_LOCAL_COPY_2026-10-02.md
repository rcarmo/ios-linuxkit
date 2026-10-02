# Cooperative local-copy example — 2 October 2026

## Scope and host

Based on default-off mainline `9c41fdbd` on `harden/cooperative-offload`.
This adds a test-only local-file handler, not a production registration or
FFmpeg integration. Source release remains 2.4.0/build817. App schemes, gadget
defaults and AOT emission settings are unchanged. No memory-policy or clustering
behaviour is imported.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 CPU cores, 16 GB-class RAM
(about 14 GiB visible), NVMe root storage, Debian Trixie AArch64, 4 KiB pages,
Clang 19.1.7. Execution is host-native, not containerised. This is Linux evidence,
not Darwin, Xcode, signing or physical-device validation.

## Handler contract

`tests/arm64/upstream/handlers/local-copy.c` is linked only by the test runner.
It uses raw guest INPUT/OUTPUT arguments and the borrowed guest filesystem
context. It does not rewrite paths, change host CWD, scan directories, create
pipes or start workers. Existing legacy handlers are untouched.

- Input must open as a regular file and be at most 1 MiB. Final symlinks are
  refused; intermediate components retain normal guest VFS semantics.
- Output must be new: `O_CREAT | O_EXCL | O_NOFOLLOW`, never `O_TRUNC`. Its guest
  mode is `0600 & ~guest_umask`; this is guest metadata, not a claim that fakefs
  backing-file permissions implement the same host-user security boundary.
- Each read/write requests at most 4 KiB; combined read/write calls are capped
  at 1,024. The handler copies only the initial bounded input size and checks
  final size. It detects growth/shrink, but offers no snapshot against same-size
  concurrent writes and no atomic publication or durability guarantee.
- Zero, negative (including EINTR/EAGAIN) and oversized transfer results fail
  without hidden retries. Close failure also fails the operation.
- Cooperative cancellation/deadline checks run between operations and after
  cleanup. `O_NONBLOCK` belongs only to freshly opened descriptors (including
  refusal of FIFO inputs without waiting for a writer). It is not a disk-latency
  guarantee: VFS path/open/stat/read/write/close operations can still block.
- Failure/cancellation retains explicitly partial output. Cleanup closes only
  owned descriptors; it never unlinks or overwrites an output path that another
  actor could have replaced. Cancellation signals win at dispatcher completion.

Exit values: usage 64, refusal 65, I/O/work-limit failure 74, cooperative
interruption 75. The dispatcher retains its sticky error and signal semantics.

## Allocation bug exposed and repaired

The actual VFS allocation fixture found that `realfs_open` and fakefs's direct
bind-open branch dereferenced a NULL `fd_create` result. Both now reserve their
descriptor before backing open, returning ENOMEM without creation/truncation
when allocation fails. Failed backing opens free the reserved descriptor and
preserve the mapped errno. No path-based rollback is added. An inode allocation
failure after successful exclusive creation retains the new empty guest-visible
output, consistent with the partial-output policy.

## Reproducible gates

```sh
make test-arm64-offload-local-copy CC=clang \
  RELEASE_BUILD_DIR=build-offload-release
```

`offload-local-copy.c` links the actual portable dispatcher and actual VFS
archives. The Linux adapter is enabled only for this fixture. All 51 modes run
on both realfs and fakefs:

- normal/empty/exact 1 MiB/short transfers, oversize/FIFO/directory/symlink/missing
  input, permissions, realfs submounts and guest umask;
- existing/same/final symlink/dangling output without clobber;
- input/output open failure, fd/inode allocation failure at both opens, direct
  fakefs bind-open descriptor allocation failure, missing read/write callbacks;
- initial/input-output/final metadata failure; read/write EIO/ENOSPC, EINTR,
  EAGAIN, zero and oversized return; failure after one output chunk;
- close failures, growth/shrink, exact 1,024-call refusal and cooperative deadline;
- usage, pending signal, read/write/close cancellation and path replacement.

Every run checks host fd counts, retained root/CWD reference balance, token
withdrawal and host CWD. Partial outputs are checked for exact size/content;
replacement retains the new path and the original descriptor's partial bytes.
Test senders are joined; handler thread creation is forbidden. Host chdir,
fchdir, opendir and pipe abort; unlink/unlinkat during the handler also abort.

Evidence: `/workspace/artifacts/ish-offload-local-copy-20261002/`.
`run-gates.sh` completed at 08:15:36 UTC, overall status 0. Release/debug and
no-emitter AOT release/debug each pass build, 102 copy cases, TCP/token,
context, filesystem, setup, upstream, full proc/exit and compatibility gates.
Both linked-AOT gates use four frozen images with emission disabled and
runtime-off parity. The 24 source/runner hashes match the completed runs.
Compatibility includes native-oracle scalar saturation (19,696 cases), FCVT,
precise load PC, proc-mem seeks, full-width seeks, regular poll and continuation.

Failed development runs are retained: unreadable fixture archive; incorrect
fsetattr declaration; allocation injection missing compiler-folded calloc;
incorrect expected deadline partial size; untracked backing output exposed by
post-open descriptor allocation; fixture forbidding bind-setup unlink. Corrected
source and fixture runs pass. A delegate attachment path was rejected due to
workspace symlink handling; subsequent read-only review timed out at 180 seconds
(earlier review also timed out at 90 seconds). No independent-review claim.

## Remaining gates

This closes the bounded local-example host gate, not the overall app plan.
Shared precise native-fault recovery and read-only target layout diagnostics are
next. An isolated Apple no-emitter scheme, observed target ABI, matching Mach-O
images, constructors/imports/mappings, signing, physical-device fault/CoW/
invalidation/lifecycle/fallback tests and performance/energy rebaseline remain
mandatory. No real FFmpeg/network handler is admitted by this evidence.

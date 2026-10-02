# Native offload transactional setup — 2 October 2026

## Scope

Worktree `/workspace/projects/ish-arm64-offload`, branch
`harden/cooperative-offload`, based on v2.4.0/build817 (`33070990`). This is a
preparatory safety tranche, not cooperative offload activation or app AOT.
No memory-pressure policy, tickets, clustering or prototype commit is imported.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four
Cortex-A520 cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, host-native
Debian Trixie AArch64, Linux 4 KiB pages; Clang 19, Meson, Ninja, Bun 1.4.2.
No Apple SDK/Xcode/signing/physical Apple device is available.

## Ownership audit

The v2.4.0 scaffold committed guest exec before unchecked forwarding thread
starts. Registry/argv/env strings and forwarding state had unchecked allocations;
pipe failure silently dropped output; pthread IDs could be uninitialised. Spawn
started forwarders after creating the child. Borrowed stdio could outlive guest
CLOEXEC closure, and direct array indexing assumed at least three slots.

The legacy path changes process-wide CWD, reconstructs file metadata by scanning
paths, and writes arbitrary guest sinks from host forwarder threads. These remain
limitations, not properties validated by setup rollback. Guest TTY poll explicitly
claims write-ready even when PTYs may block. Changing a shared fd's O_NONBLOCK
flag or timing out a join cannot establish safe bounded cleanup. Future
cooperative handlers need explicit guest filesystem access and a per-call
nonblocking/cancellable output contract; no detached token/task-using workers.

## Changes

- Registry names/handlers and every allocated argv/env string are checked;
  partial arrays are freed. Registry publication follows successful allocation.
- Internal pipes are CLOEXEC and moved above host fd2 before spawn actions.
- Forwarder startup returns an error and transfers read-end ownership only
  after successful pthread creation. Failed starts refund guest fd references
  and free state. Caller tracks thread liveness explicitly, not pthread_t zero.
- Exec-surviving stdio is snapshotted with retained references, respecting table
  bounds and guest CLOEXEC. Missing output is represented as closed handler fds;
  spawn uses /dev/null rather than inheriting unrelated host descriptors.
- All handler setup (including legacy host-CWD resolution/open/chdir) precedes
  exec commitment. Rollback closes writers, joins already-started EOF readers,
  closes untransferred readers, releases references and allocations. No handler
  output is produced during this rollback.
- Spawn file actions/attributes and every setup action are checked; both
  forwarders exist before spawn. A spawn failure uses the same unwind. After
  spawn success no setup error can return and strand a child. Unexpected join
  or reap failures stop execution rather than detach owned workers/children.
- Return codes are masked to eight bits before shifting; zero output writes no
  longer spin. Legacy file-scan CWD buffer is thread-local.

The existing portable Linux build remains unsupported by default. Fixture-only
macros expose the real portable handler source and POSIX spawn path on Linux.
Darwin executable discovery/dylib injection is not modelled or certified by them.

## Regression gate

```sh
CC=clang EVIDENCE_DIR=/absolute/new-evidence make test-arm64-offload-setup \
  RELEASE_BUILD_DIR=build-offload-release
```

`offload-setup.c` links actual candidate archives and portable source. Linker
wrappers inject each allocation, pipe, descriptor flag, legacy CWD open/change,
thread creation and spawn-action failure position. Each error asserts:

- guest comm/did_exec/signal dispositions/altstack unchanged;
- identical host descriptor count and CWD;
- exact original guest fd reference counts;
- zero live tracked setup allocations;
- no handler invocation and no child left to reap.

Each enumeration ends with a successful invocation to detect stale state.
Success covers both stdout/stderr, short writes, negative return masking,
CLOEXEC stdio closure and host-fd0-closed pipe relocation. Actual Linux
posix_spawn of /bin/true tests child ownership and reap; no host-thread kill or
forced cancellation is used.

Four matching gadget release/debug and no-emitter AOT release/debug builds use
unchanged frozen images. Evidence is retained in
`/workspace/artifacts/ish-offload-hardening-20261002/`. Authoritative suites use
`gates-<lane>/{setup,upstream,proc,linked}.rc`; source/runner hashes freeze the
test versions. All four setup/upstream/proc suites pass, including native plus
 two 25 s guest proc runs per lane at 16 forkers/six readers. Both linked-AOT
 suites pass with all four frozen images and zero emission. Documentation
 links/style/tests pass. This preparatory tranche is committed only on the
 isolated branch; the full cooperative tranche is not yet eligible for mainline
 merge.

## Remaining gates

Replace process-wide CWD for cooperative handlers with a retained guest fs
context using generic_openat/path-normalisation and guest fd operations. Do not
expose raw fakefs backing paths as a substitute for mount/symlink/permission and
metadata semantics. Add bounded I/O/backpressure, signal-token lifecycle,
concurrency and cancellation cleanup tests before admitting a real handler.
Apple native fault integration, ABI-bound Mach-O generation, signing and device
measurement remain separate app-AOT work. A delegated read-only sink/filesystem
audit timed out; no independent-review claim.

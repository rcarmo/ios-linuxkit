# Offload guest filesystem context foundation — 2 October 2026

## Scope

Isolated `harden/cooperative-offload` branch, after setup hardening `27a2f383`.
This introduces a filesystem helper for future cooperative handlers; existing
legacy handlers retain their contract. No new handler is registered, no token
API imported and no app AOT/native emitter enabled. Mainline remains v2.4.0.
The sidebar CWD step is not complete until cooperative execution actually uses
this helper and avoids raw argv translation, chdir and post-hoc metadata scans.

Host: Orange Pi 6 Plus/CIX P1, eight Cortex-A720/four Cortex-A520 cores,
16 GB-class RAM, NVMe/ext4, host-native Debian Trixie AArch64, 4 KiB pages.
No Apple SDK/device proof is available.

## Design

`kernel/native_fs.c` snapshots root/CWD descriptor references and umask under
`fs_info.lock`, with checked context/snapshot allocation. The caller is the
owning guest task and pthread; other host workers must not use this context.
Opening through the helper returns a guest `struct fd`, not a host pathname or
borrowed host descriptor. Ownership lasts until explicit destroy before handler
return. No process-wide current->fs replacement is needed.

`generic_open_in_fs` shares the existing generic open implementation and uses
explicit snapshot path normalization. Existing generic_openat retains its path.
The explicit path constrains dot-dot at the snapshot root and resolves absolute
symlinks from that root. Mount selection, fakefs open/metadata, existing access
checks, device handling and O_NOFOLLOW use the real VFS. Permissions inherit
existing VFS behaviour; this is not an independent hardened filesystem sandbox,
mount-namespace snapshot or global pathwalk/race repair. Mount changes remain
visible. Open/create and disk I/O have no general wall-clock deadline.

Root/CWD references are retained, not path strings. The snapshot ignores later
CWD changes in the source fs_info. Umask is applied to create modes. Context
allocation failures release partial state and preserve descriptor refcounts.

## Verification

```sh
CC=clang EVIDENCE_DIR=/absolute/new-context-evidence make test-arm64-native-fs \
  RELEASE_BUILD_DIR=build-offload-release
```

Fixture links actual candidate archives, compiled with matching native-frame
macros. Both realfs and imported fakefs roots exercise:

- separate CWD snapshots and concurrent guest-task callers in different dirs;
- relative/dot-dot and absolute paths, absolute symlinks, O_NOFOLLOW rejection;
- nondefault root, root-bounded dot-dot and absolute symlink resolution;
- mounted-path selection using actual mount table;
- output creation with guest umask and immediately visible fakefs metadata;
- non-root denied file read using existing guest permissions;
- both context allocation failure positions, descriptor-ref preservation;
- foreign host-thread/context rejection, host CWD unchanged.

All four gadget release/debug and no-emitter AOT release/debug context suites
pass in `context-<lane>-final.rc` under
`/workspace/artifacts/ish-offload-hardening-20261002/`. Full setup/upstream/proc
and linked-AOT gates are rerun after adding the context/path code; their logs
and status timestamps supersede pre-context results. All four setup/upstream
and full proc gates pass, with both frozen-image gates required before commit.
No images are retrained.

Early fixture errors are retained in `context/`: missing internal header macro,
missing AT_PWD declaration, wrong printf format, and closing a CWD after passing
ownership to fs_chdir caused a dangling fixture descriptor. Corrected fixtures
compile with -Wall/-Wextra/-Werror and pass. No independent-review claim.

## Next

Wire this retained context into the opt-in cooperative executor. Keep raw guest
argv and let handlers open paths explicitly; do not scan host dirs afterwards.
Bounded stream/output admission must precede real handlers. Then extract locked
signal-token ownership and complete four-lane cancellation/concurrency gates.
App AOT target ABI/signing/device validation remains separate and unavailable
on this host.

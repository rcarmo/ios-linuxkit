# Native offload contracts

Native offload substitutes a registered host handler or executable for selected
guest commands. It executes with host-process privileges; it adds no sandbox.
Registration must finish at startup, before the first guest thread launches or
runs. The registry stays frozen even if that first launch fails. Ordinary Linux
builds reject native offload; Linux fixtures explicitly compile the actual
portable dispatcher with test-only adapter definitions.

The API is in `kernel/native_offload.h`. No production cooperative handler is
registered by the supplied app schemes. The existing FFmpeg test scheme still
uses the legacy fake handler; it is not real FFmpeg or a cooperative handler.

## Select a contract

| Contract | Registration | Arguments/filesystem | I/O and lifetime |
|---|---|---|---|
| Legacy in-process | `native_offload_add_handler` | Host-path translation, process-wide host CWD, later fakefs metadata scans. | Pipe forwarders; no bounded cancellation guarantee. |
| macOS host executable | `native_offload_add` / CLI `-n NAME=PATH` | Existing host-path/redirect-library integration. | Checked spawn setup, owned forwarders and child reap. |
| Cooperative in-process | `native_offload_add_cooperative_handler` | Raw guest argv and retained guest VFS root/CWD/umask; no host chdir, rewriting or directory scans. | Guest-thread-owned borrowed context; explicit checkpoints; restricted stream restrictions. |

Generic `ffmpeg`/`ffprobe` offloads match exact `/bin`, `/usr/bin` or
`/usr/local/bin` executable paths. Relative/private paths and readable shebang
wrappers use guest execution. `NO_OFFLOAD=1` or `MINIS_NO_FFMPEG_OFFLOAD=1` in
the guest environment disables generic selection. Synthetic command names keep
their existing policy. These checks do not validate arguments or host paths.

Setup allocates and retains resources before committing exec state. Allocation,
pipe, descriptor, forwarder and spawn-action failures release owned resources.
After commitment, normal completion uses guest exit status; negative handler
returns are masked to an eight-bit exit code. Legacy contracts still have
blocking sinks and process-wide side effects. Transactional setup does not
make their successful I/O or cancellation bounded.

## Cooperative ownership and filesystem access

A cooperative handler runs on the calling guest task/pthread. Execution rejects
a guest group with siblings or one already exiting with EBUSY. Its
`native_handler_context` and filesystem context are borrowed until return and
must not escape to workers. The framework creates no pipes or workers for this
path. Any handler-owned workers must finish and be joined before return; never
use detached task/token workers, `pthread_cancel` or async cancellation.

Use `native_handler_fs(context)` and `native_fs_open` with guest flags from
`kernel/fs.h`. Opens return owned guest `struct fd` references; close them before
return. The retained root, CWD and umask preserve guest mount, permission,
symlink and fakefs metadata semantics even if another caller changes its CWD.
No host path is returned. New realfs/fakefs backing opens reserve the descriptor
before open/create/truncate, so descriptor allocation refusal has no backing-file
effect. Later failure can leave a new partial file.

`native_handler_check` polls cancellation and the execution checkpoint deadline.
Tokens remain published through cleanup and are withdrawn under `sighand.lock`.
Eligible unblocked, nonignored INT/TERM/HUP/QUIT requests are sticky; SIGKILL
has priority. Forked children clear native token/proxy/pid state. Signals win at
completion; other pending signals retain their ordinary queue semantics.

## Restricted stream restrictions

Only connected INET/INET6 TCP stdio using the actual guest socket operations is
admitted. Unconnected sockets, datagrams, Unix sockets, custom backends, TTYs,
files and pipes are refused before exec commitment. Closed or CLOEXEC stdio is
retained as an absent stream, never inherited from host stdio; attempts to use it
return EBADF. A handler needing only VFS files can run with absent stdio; that
does not admit arbitrary file or terminal stream operations.

- Each call requests at most 4,096 bytes.
- Input has a 1 MiB budget; stdout/stderr share another 1 MiB budget.
- A stream call may retry for at most 250 ms; `wait_ms=0` allows immediate EAGAIN.
- Execution checks use a 5,000 ms checkpoint deadline, not a watchdog.
- Calls use per-call `MSG_DONTWAIT`, never shared `O_NONBLOCK` mutation. Linux
  uses `MSG_NOSIGNAL`; Darwin requires existing `SO_NOSIGPIPE`, checked without
  changing the socket.
- Short transfers belong to the caller. Fatal I/O/deadline/budget errors remain
  sticky and force failure even if the callback ignores them.

Poll readiness alone does not bound the following write. Apple Terminal/UI
writes and PTYs can block while ignoring cancellation, so they are not admitted
by this stream contract. Disk/VFS open/stat/read/write/close operations also have
no universal latency bound. Checkpoints cannot interrupt an operation that does
not return.

## Test-only local-copy example

`tests/arm64/upstream/handlers/local-copy.c` is linked only by its runner, never
by normal app or CLI builds. It copies a regular input of at most 1 MiB, requests
at most 4 KiB per transfer and caps combined read/write calls at 1,024. It checks
initial/final size but offers no snapshot against same-size concurrent writes.

Output must be exclusively new (`O_CREAT | O_EXCL | O_NOFOLLOW`, no truncation),
with guest mode `0600 & ~umask`. Final symlinks are refused. Error or cancellation
retains partial output, including an empty newly created file. Cleanup only
closes owned descriptors: it never unlinks or overwrites a potentially replaced
output path. There is no atomic publication or durability guarantee. Fakefs's
guest mode does not define a separate host-user security boundary.

Zero, negative (including EINTR/EAGAIN) or oversized transfers fail without
hidden retries. Exit codes are usage 64, refusal 65, I/O/work-limit failure 74
and interruption 75; cancellation signals take precedence in the dispatcher.
This example does not enable real FFmpeg or a general network/file handler.

## Validation

```sh
make build-arm64-linux CC=clang
make test-arm64-offload-setup test-arm64-native-fs \
  test-arm64-offload-context test-arm64-offload-io \
  test-arm64-offload-local-copy CC=clang
```

The actual-source Linux adapters cover setup rollback, VFS concurrency and
ownership, 39 TCP dispatcher modes, 1,000 signal/completion races and 51
local-copy modes on each of realfs/fakefs. Use matching gadget/native build
archives. [Validation](VALIDATION.md) supplies broader release/debug, procfs,
compatibility and frozen-AOT checks. Dated evidence is in the
[setup](reports/audits/OFFLOAD_SETUP_2026-10-02.md),
[filesystem](reports/audits/OFFLOAD_FS_CONTEXT_2026-10-02.md),
[context](reports/audits/OFFLOAD_CONTEXT_EXEC_2026-10-02.md),
[TCP/token](reports/audits/OFFLOAD_BOUNDED_IO_2026-10-02.md) and
[local-copy](reports/audits/OFFLOAD_LOCAL_COPY_2026-10-02.md) reports.
Darwin/Xcode/signing/device checks require Apple tools and hardware; Linux
adapter passes do not satisfy them.

#ifndef NATIVE_OFFLOAD_H
#define NATIVE_OFFLOAD_H

#include <stdbool.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <signal.h>
struct task;

// ============================================================================
// Native Offload — bypass emulation for selected guest binaries
// ============================================================================
//
// When guest execve() matches a registered binary name, execution is
// intercepted and routed to either:
//   1. An in-process handler function (iOS + macOS)
//   2. A native host binary via posix_spawn (macOS only)
//
// The handler receives argc/argv (with guest paths translated to host paths)
// and pipe-backed stdio fds. Output written to stdout_fd/stderr_fd is
// forwarded through the guest TTY driver, so it appears in the terminal UI.
//
// --- Quick integration guide ---
//
//   // 1. Define your handler (runs on guest thread, in-process)
//   static int my_tool_main(int argc, char **argv,
//                           int stdin_fd, int stdout_fd, int stderr_fd) {
//       dprintf(stdout_fd, "hello from native handler\n");
//       return 0; // exit code
//   }
//
//   // 2. Register it (call once at app startup, before guest runs)
//   native_offload_add_handler("mytool", my_tool_main);
//
//   // 3. Done — when guest runs `execve("/usr/bin/mytool", ...)`,
//   //    my_tool_main() is called instead of emulating the ELF binary.
//
// Notes for handler authors:
//   - argv paths starting with '/' are auto-translated to host filesystem
//   - CWD is set to the host equivalent of the guest's CWD
//   - Use dprintf(stdout_fd, ...) and dprintf(stderr_fd, ...) for output
//   - stdin_fd may be -1 if guest stdin is not backed by a real host fd
//   - Return 0 for success, non-zero for failure (becomes guest exit code)
//   - Handler runs on the guest thread; do_exit() is called after return
//
// macOS CLI also supports host binary offload:
//   ish -n ffmpeg                     # auto-detect /opt/homebrew/bin/ffmpeg
//   ish -n ffprobe=/usr/local/bin/ffprobe  # explicit path
//
// ============================================================================

#define NATIVE_OFFLOAD_MAX 32

// Handler function signature. Called in-process on the guest thread.
// Receives translated argv (guest absolute paths → host paths) and
// pipe-backed stdio fds for output.
// Must return an exit code (0 = success).
typedef int (*native_handler_func)(int argc, char **argv,
                                   int stdin_fd, int stdout_fd, int stderr_fd);

// Register an in-process handler for a guest binary name.
// When guest execve() basename matches guest_name, handler is called
// instead of emulating the binary. Takes priority over host binary lookup.
// Returns 0 on success, -1 if registry is full.
int native_offload_add_handler(const char *guest_name, native_handler_func handler);

// Explicit guest filesystem context for cooperative handlers. Owned by
// the calling guest thread; never use it from an unregistered host worker or
// after returning. Opens preserve guest VFS metadata/mount/path semantics and
// return guest struct fd objects, not host paths. File I/O is not deadline-safe.
struct native_fs_context;
struct fd;
struct native_fs_context *native_fs_context_create(void);
void native_fs_context_destroy(struct native_fs_context *context);
struct fd *native_fs_open(struct native_fs_context *context, const char *path,
        int flags, int mode);

// Isolated cooperative execution scaffold. Register before guest execution
// starts, at app startup only. No production handler is enabled by default.
// Unlike the legacy contract, argv is raw guest argv, host CWD is untouched and
// all filesystem access uses native_handler_fs(context). No host path rewriting
// or post-hoc directory/metadata scan takes place. The context is borrowed until
// return, guest-thread owned, and must not escape to workers. Open fds are owned
// by the handler and must be closed before return. Poll native_handler_check at
// bounded intervals. All handler-owned workers must be joined before return;
// the framework creates no workers and never uses pthread_cancel. Filesystem
// opens/disk I/O are NOT deadline-safe. Unsupported stdio is refused precommit.
// Execution refuses a multi-thread guest group or an exiting group.
struct native_handler_context;
typedef int (*native_cooperative_handler_func)(int argc, char **argv,
        struct native_handler_context *context);
int native_offload_add_cooperative_handler(const char *guest_name,
        native_cooperative_handler_func handler);
struct native_fs_context *native_handler_fs(struct native_handler_context *context);
// Streams: connected TCP sockets only (actual guest socket fdops, INET/INET6).
// No shared O_NONBLOCK mutation, borrowed host fd, TTY/file/pipe fallback or
// Unix-socket SCM bypass. Closed/CLOEXEC stdio returns EBADF. Darwin output must
// already have SO_NOSIGPIPE; it is checked, not silently changed. Linux uses
// MSG_NOSIGNAL. All calls execute on the owning guest thread, not workers.
#define NATIVE_IO_CHUNK 4096
#define NATIVE_IO_BYTES (1024 * 1024) // stdin and combined stdout/stderr budgets
#define NATIVE_IO_WAIT_MS 250        // maximum per-call retry deadline
#define NATIVE_HANDLER_MS 5000      // checkpoints/stream calls, not a watchdog
int native_handler_check(struct native_handler_context *context);
int native_handler_signal(struct native_handler_context *context);
ssize_t native_handler_read(struct native_handler_context *context, void *buf,
        size_t size, unsigned wait_ms);
ssize_t native_handler_write(struct native_handler_context *context, unsigned stream,
        const void *buf, size_t size, unsigned wait_ms);
// Short transfers must be handled by the caller. Fatal I/O/time/budget errors
// are sticky and force nonzero guest exit even if ignored; signal status wins.
// Invalid calls (wrong context/stream/size) do not mutate context state.

// Opt-in locked signal-token lifecycle, independent of any stream backend.
struct native_cancel { atomic_int signal; };
int native_cancel_signal(const struct native_cancel *cancel);
bool native_cancel_begin(struct task *task, struct native_cancel *cancel);
int native_cancel_finish(struct task *task, struct native_cancel *cancel);
bool native_cancel_request(struct task *task, int sig);
// Caller holds sighand->lock; task/token lifetimes remain owned by executor.
bool native_cancel_request_locked(struct task *task, int sig);

// Internal startup boundary; called before launching/running the first guest.
void native_offload_freeze_registry(void);

// Register a host binary offload (macOS CLI only, uses posix_spawn).
// spec is "name" or "name=/host/path". Returns 0 on success.
int native_offload_add(const char *spec);

// Check if a guest binary should be offloaded.
// Returns native host path, "[builtin]" for handler-only, or NULL.
const char *native_offload_lookup(const char *guest_path);

// Exec-aware lookup: restrict ffmpeg/ffprobe to exact system bin paths and
// honour packed-env opt-outs. Exec separately rejects generic shebang scripts.
const char *native_offload_lookup_exec(const char *guest_path, const char *envp,
                                       bool *generic_out);

// Execute the offloaded binary (handler or posix_spawn).
// Takes over the current guest task and calls do_exit(). Does not return
// on success. Returns negative errno on failure.
int native_offload_exec(const char *native_path,
                        const char *guest_file,
                        size_t argc, const char *argv,
                        const char *envp);

// Forward a signal to the native process backing a proxy task.
// Returns true if the signal was forwarded.
bool native_offload_forward_signal(struct task *task, int sig);

#endif

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

// Opt-in cooperative handlers must poll at bounded intervals, interrupt their
// own blocking I/O, close resources and join token-using workers before return.
// No pthread_cancel or host signal is delivered to an in-process handler.
// Cooperative exec requires a single-thread guest group not already exiting.
// Registration is startup-only. This is not full guest signal/process semantics.
struct native_cancel {
    atomic_int signal; // read through native_cancel_signal(); sticky, KILL wins
};
typedef int (*native_cooperative_handler_func)(int argc, char **argv,
        int stdin_fd, int stdout_fd, int stderr_fd, const struct native_cancel *cancel);
int native_offload_add_cooperative_handler(const char *guest_name,
        native_cooperative_handler_func handler);
int native_cancel_signal(const struct native_cancel *cancel);
// Internal lifecycle: caller owns the token until finish, task lifetime is
// protected, and sighand remains attached. Publication/withdrawal use its lock.
bool native_cancel_begin(struct task *task, struct native_cancel *cancel);
int native_cancel_finish(struct task *task, struct native_cancel *cancel);
bool native_cancel_request(struct task *task, int sig);
// Signal-queue integration; caller already holds sighand->lock.
bool native_cancel_request_locked(struct task *task, int sig);

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

// Linux adapter links actual Apple portable-handler path; not Apple validation.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "fs/fd.h"
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/native_offload.h"
#include "fs/real.h"
static jmp_buf finished;
static unsigned calls;
static bool cancel_case;
static bool output_case;
static bool drain_case;
static int cancel_signal = SIGTERM_;
static int saved_stdout;
static struct task *target;
static char directory[] = "/tmp/ish-native-handler-XXXXXX";
static struct fd *output_fd;
static ssize_t output_write(struct fd *fd, const void *buf, size_t n) {
    if (drain_case) send_signal(target, SIGTERM_, SIGINFO_NIL);
    return write(fd->real_fd, buf, n);
}
static int output_close(struct fd *fd) { return close(fd->real_fd); }
static struct fd_ops output_ops = {.write = output_write, .close = output_close};
noreturn void __wrap_do_exit(int status) {
    assert(current->native_cancel == NULL);
    assert(status == (cancel_case ? cancel_signal : 7 << 8));
    if (output_case) {
        // Forwarders must have reached EOF, joined and released their fd refs.
        assert(output_fd->refcount == 1);
        FILE *f = fopen(directory, "r"); assert(f);
        char buf[32]; assert(fgets(buf, sizeof(buf), f));
        assert(!strcmp(buf, "handler-output\n")); fclose(f);
    }
    longjmp(finished, 1);
}
static int handler(int argc, char **argv, int in, int out, int err,
        const struct native_cancel *cancel) {
    (void)in; (void)err;
    assert(argc == 1 && strstr(argv[0], "probe"));
    assert(native_cancel_signal(cancel) == 0);
    if (output_case) assert(write(out, "handler-output\n", 15) == 15);
    // In drain mode the forwarder may already have requested cancellation.
    calls++;
    if (cancel_case && !drain_case) {
        send_signal(current, SIGTERM_, SIGINFO_NIL);
        if (cancel_signal == SIGKILL_) send_signal(current, SIGKILL_, SIGINFO_NIL);
        assert(native_cancel_signal(cancel) == cancel_signal);
    }
    return 7;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    cancel_case = !strcmp(argv[1], "cancel") || !strcmp(argv[1], "kill");
    if (!strcmp(argv[1], "kill")) cancel_signal = SIGKILL_;
    bool busy = !strcmp(argv[1], "busy");
    bool exiting = !strcmp(argv[1], "exiting");
    drain_case = !strcmp(argv[1], "drain");
    output_case = drain_case || !strcmp(argv[1], "output");
    bool pending = !strcmp(argv[1], "pending");
    if (pending || drain_case) cancel_case = true;
    assert(pending || cancel_case || output_case || busy || exiting || !strcmp(argv[1], "normal"));
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    target = current;
    if (output_case) {
        saved_stdout = dup(STDOUT_FILENO); assert(saved_stdout >= 0);
        // Populate fd 0/1/2, then replace stdout with a real-backed guest file.
        assert(create_piped_stdio() == 0);
        int fd = mkstemp(directory); assert(fd >= 0);
        output_fd = fd_create(&output_ops); assert(output_fd);
        output_fd->real_fd = fd;
        fd_close(current->files->files[1]);
        current->files->files[1] = output_fd;
    }
    assert(native_offload_add_cooperative_handler("probe", handler) == 0);
    if (busy || exiting) {
        struct task peer = {0};
        lock(&pids_lock); lock(&current->group->lock);
        if (busy) list_add(&current->group->threads, &peer.group_links);
        current->group->doing_group_exit = exiting;
        unlock(&current->group->lock); unlock(&pids_lock);
        assert(native_offload_exec("[builtin]", "/bin/probe", 1, "/bin/probe\0", "") == _EBUSY);
        assert(!calls && !current->native_cancel && !current->did_exec);
        lock(&pids_lock); lock(&current->group->lock);
        if (busy) list_remove(&peer.group_links);
        current->group->doing_group_exit = false;
        unlock(&current->group->lock); unlock(&pids_lock);
        puts("native-cooperative-handler-ok"); return 0;
    }
    if (pending) send_signal(current, SIGTERM_, SIGINFO_NIL);
    if (setjmp(finished) == 0) {
        const char packed[] = "/bin/probe\0";
        native_offload_exec("[builtin]", "/bin/probe", 1, packed, "");
        assert(0);
    }
    assert(calls == !pending);
    if (output_case) {
        current->files->files[1] = NULL;
        fd_close(output_fd); unlink(directory);
        assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO); close(saved_stdout);
    }
    puts("native-cooperative-handler-ok");
}

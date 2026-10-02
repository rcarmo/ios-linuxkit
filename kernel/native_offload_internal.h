#ifndef NATIVE_OFFLOAD_INTERNAL_H
#define NATIVE_OFFLOAD_INTERNAL_H
// Private guest-thread-owned execution state. Never passed to host workers.
#include "kernel/native_offload.h"
#include "kernel/task.h"
#include "fs/fd.h"
struct native_handler_context {
    struct task *owner;
    pthread_t thread;
    struct native_fs_context *fs;
    struct fd *stdio[3];
    struct native_cancel cancel;
    uint64_t deadline_ns;
    size_t input_left, output_left;
    int io_error;
};
int native_io_admit(struct fd *fd, bool output);
int native_io_init(struct native_handler_context *context);
#endif

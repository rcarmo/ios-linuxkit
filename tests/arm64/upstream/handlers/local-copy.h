#ifndef TEST_NATIVE_LOCAL_COPY_H
#define TEST_NATIVE_LOCAL_COPY_H
#include "kernel/native_offload.h"
// Test/example only: never linked or registered by an app/CLI build.
// INPUT/OUTPUT are guest paths. OUTPUT must be new, 0600 & guest umask.
// Failure/cancellation retains partial output; never deletes/replaces a path.
// No atomic publication, source snapshot or durable storage promise.
#define NATIVE_COPY_MAX (1024 * 1024)
#define NATIVE_COPY_OPS 1024
#define NATIVE_COPY_USAGE 64
#define NATIVE_COPY_REFUSED 65
#define NATIVE_COPY_IO 74
#define NATIVE_COPY_INTERRUPTED 75
int native_local_copy(int argc, char **argv, struct native_handler_context *context);
#endif

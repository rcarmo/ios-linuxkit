// Work-bounded local copy example, intentionally NOT a disk-latency guarantee.
#include <sys/stat.h>
#include <string.h>
#include "local-copy.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/fd.h"
static int checkpoint(struct native_handler_context *context) {
    return native_handler_check(context) < 0 ? NATIVE_COPY_INTERRUPTED : 0;
}
static int regular(struct fd *fd, struct statbuf *stat) {
    if (!fd->mount || !fd->mount->fs->fstat) return _EOPNOTSUPP;
    int err = fd->mount->fs->fstat(fd, stat);
    if (err < 0) return err;
    if (!S_ISREG(stat->mode) || !S_ISREG(fd->type)) return _EOPNOTSUPP;
    return 0;
}
int native_local_copy(int argc, char **argv, struct native_handler_context *context) {
    if (argc != 3 || !argv || !argv[1] || !argv[2] || !*argv[1] || !*argv[2] ||
            strlen(argv[1]) >= MAX_PATH || strlen(argv[2]) >= MAX_PATH) return NATIVE_COPY_USAGE;
    int status = checkpoint(context);
    if (status) return status;
    struct native_fs_context *fs = native_handler_fs(context);
    if (!fs) return NATIVE_COPY_REFUSED;
    // O_NONBLOCK affects this newly owned descriptor only (FIFO open must not
    // wait). Final symlinks are refused; earlier path components use guest VFS.
    struct fd *input = native_fs_open(fs, argv[1], O_RDONLY_ | O_NOFOLLOW_ | O_NONBLOCK_, 0);
    if (IS_ERR(input)) return NATIVE_COPY_REFUSED;
    struct fd *output = NULL;
    struct statbuf before;
    status = NATIVE_COPY_REFUSED;
    if (regular(input, &before) < 0 || before.size > NATIVE_COPY_MAX || !input->ops->read)
        goto cleanup;
    status = checkpoint(context);
    if (status) goto cleanup;
    output = native_fs_open(fs, argv[2], O_WRONLY_ | O_CREAT_ | O_EXCL_ |
        O_NOFOLLOW_ | O_NONBLOCK_, 0600);
    if (IS_ERR(output)) { output = NULL; status = NATIVE_COPY_REFUSED; goto cleanup; }
    struct statbuf target;
    status = NATIVE_COPY_REFUSED;
    if (regular(output, &target) < 0 || !output->ops->write) goto cleanup;
    // Fixed total data/work limits even if another task changes the input.
    // Zero/EINTR/EAGAIN is failure, never an unbounded hidden retry here.
    char buf[NATIVE_IO_CHUNK];
    size_t total = 0;
    unsigned operations = 0;
    status = NATIVE_COPY_IO;
    while (total < before.size) {
        if (++operations > NATIVE_COPY_OPS) goto cleanup;
        status = checkpoint(context);
        if (status) goto cleanup;
        size_t size = before.size - total;
        if (size > sizeof(buf)) size = sizeof(buf);
        ssize_t n = input->ops->read(input, buf, size);
        status = NATIVE_COPY_IO;
        if (n <= 0 || (size_t)n > size) goto cleanup;
        size_t written = 0;
        while (written < (size_t)n) {
            if (++operations > NATIVE_COPY_OPS) goto cleanup;
            status = checkpoint(context);
            if (status) goto cleanup;
            ssize_t w = output->ops->write(output, buf + written, (size_t)n - written);
            status = NATIVE_COPY_IO;
            if (w <= 0 || (size_t)w > (size_t)n - written) goto cleanup;
            written += (size_t)w;
        }
        total += (size_t)n;
    }
    status = checkpoint(context);
    if (status) goto cleanup;
    // Detect shrink/growth; not a guarantee of stable data against concurrent
    // same-size writes. Copy exactly the initial bounded size, never new bytes.
    struct statbuf after;
    status = NATIVE_COPY_IO;
    if (regular(input, &after) < 0 || after.size != before.size) goto cleanup;
    status = checkpoint(context);
cleanup:
    // Never path-based rollback: an actor could replace/unlink OUTPUT while
    // we hold its original descriptor. Close only our owned references.
    if (output && fd_close(output) < 0 && !status) status = NATIVE_COPY_IO;
    if (fd_close(input) < 0 && !status) status = NATIVE_COPY_IO;
    int interrupted = checkpoint(context);
    return interrupted ? interrupted : status;
}

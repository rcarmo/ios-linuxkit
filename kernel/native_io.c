// Restricted per-call nonblocking streams. No poll trust, flags or workers.
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include "kernel/native_offload_internal.h"
#include "kernel/errno.h"
#include "fs/sock.h"
extern const struct fd_ops socket_fdops;

static bool owned(struct native_handler_context *context) {
    return context && current == context->owner &&
        pthread_equal(pthread_self(), context->thread);
}
static int now_ns(uint64_t *now) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return errno_map();
    *now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    return 0;
}
int native_io_init(struct native_handler_context *context) {
    uint64_t now;
    int err = now_ns(&now);
    if (err < 0) return err;
    context->deadline_ns = now + (uint64_t)NATIVE_HANDLER_MS * 1000000;
    context->input_left = context->output_left = NATIVE_IO_BYTES;
    return 0;
}
int native_io_admit(struct fd *fd, bool output) {
    if (!fd) return 0; // Closed stream remains EBADF, never inherited host stdio.
    if (fd->ops != &socket_fdops || fd->real_fd < 0 ||
            fd->socket.type != SOCK_STREAM_ ||
            (fd->socket.domain != AF_INET_ && fd->socket.domain != AF_INET6_))
        return _EOPNOTSUPP;
    int type;
    socklen_t len = sizeof(type);
    if (getsockopt(fd->real_fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0) return errno_map();
    if (type != SOCK_STREAM) return _EOPNOTSUPP;
    struct sockaddr_storage peer;
    len = sizeof(peer);
    if (getpeername(fd->real_fd, (struct sockaddr *)&peer, &len) < 0) return errno_map();
    if ((fd->socket.domain == AF_INET_ && peer.ss_family != AF_INET) ||
            (fd->socket.domain == AF_INET6_ && peer.ss_family != AF_INET6)) return _EOPNOTSUPP;
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    if (output) {
        int suppressed = 0;
        len = sizeof(suppressed);
        if (getsockopt(fd->real_fd, SOL_SOCKET, SO_NOSIGPIPE, &suppressed, &len) < 0)
            return errno_map();
        if (!suppressed) return _EOPNOTSUPP;
    }
#elif !defined(MSG_NOSIGNAL)
    if (output) return _EOPNOTSUPP;
#else
    (void)output;
#endif
    return 0;
}
struct native_fs_context *native_handler_fs(struct native_handler_context *context) {
    return owned(context) ? context->fs : NULL;
}
int native_handler_signal(struct native_handler_context *context) {
    return owned(context) ? native_cancel_signal(&context->cancel) : _EPERM;
}
static int fail(struct native_handler_context *context, int error) {
    if (!context->io_error) context->io_error = error;
    return error;
}
int native_handler_check(struct native_handler_context *context) {
    if (!owned(context)) return _EPERM;
    if (native_cancel_signal(&context->cancel)) return _ECANCELED;
    if (context->io_error) return context->io_error;
    uint64_t now;
    int err = now_ns(&now);
    if (err < 0) return fail(context, err);
    if (now >= context->deadline_ns) return fail(context, _ETIMEDOUT);
    return 0;
}
static ssize_t transfer(struct native_handler_context *context, unsigned stream,
        void *buf, size_t size, unsigned wait_ms) {
    if (!owned(context)) return _EPERM;
    if (stream > 2 || size > NATIVE_IO_CHUNK || wait_ms > NATIVE_IO_WAIT_MS ||
            (size && !buf)) return _EINVAL;
    int err = native_handler_check(context);
    if (err < 0) return err;
    struct fd *fd = context->stdio[stream];
    if (!fd) return fail(context, _EBADF);
    if (!size) return 0;
    size_t *left = stream ? &context->output_left : &context->input_left;
    if (size > *left) return fail(context, _EFBIG);
    uint64_t now;
    err = now_ns(&now);
    if (err < 0) return fail(context, err);
    uint64_t deadline = now + (uint64_t)wait_ms * 1000000;
    if (deadline > context->deadline_ns) deadline = context->deadline_ns;
    bool first = true;
    for (;;) {
        err = native_handler_check(context);
        if (err < 0) return err;
        if (!first) {
            err = now_ns(&now);
            if (err < 0) return fail(context, err);
            if (now >= deadline) return fail(context, _ETIMEDOUT);
        }
        first = false;
        // Single syscall, no hidden EINTR retry or shared O_NONBLOCK mutation.
        ssize_t n;
        if (stream) {
            int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            n = send(fd->real_fd, buf, size, flags);
        } else {
            n = recv(fd->real_fd, buf, size, MSG_DONTWAIT);
        }
        if (n > 0) { *left -= (size_t)n; return n; }
        if (n == 0) return stream ? fail(context, _EIO) : 0;
        int host_error = errno;
        if (host_error != EAGAIN && host_error != EWOULDBLOCK && host_error != EINTR)
            return fail(context, err_map(host_error));
        err = native_handler_check(context);
        if (err < 0) return err;
        err = now_ns(&now);
        if (err < 0) return fail(context, err);
        if (!wait_ms) return _EAGAIN;
        if (now >= deadline) return fail(context, _ETIMEDOUT);
        // Small monotonic retry slices, no readiness callback or task-using worker.
        uint64_t pause = deadline - now;
        if (pause > 1000000) pause = 1000000;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)pause};
        if (nanosleep(&delay, NULL) < 0 && errno != EINTR) return fail(context, errno_map());
    }
}
ssize_t native_handler_read(struct native_handler_context *context, void *buf,
        size_t size, unsigned wait_ms) {
    return transfer(context, 0, buf, size, wait_ms);
}
ssize_t native_handler_write(struct native_handler_context *context, unsigned stream,
        const void *buf, size_t size, unsigned wait_ms) {
    if (stream != 1 && stream != 2) return _EINVAL;
    return transfer(context, stream, (void *)buf, size, wait_ms);
}

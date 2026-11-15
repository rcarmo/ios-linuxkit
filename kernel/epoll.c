#include <sys/stat.h>
#include <limits.h>
#include "kernel/calls.h"
#include "fs/poll.h"

static struct fd_ops epoll_ops;

fd_t sys_epoll_create(int_t flags) {
    STRACE("epoll_create(%#x)", flags);
    if (flags & ~(O_CLOEXEC_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&epoll_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll)) {
        fd_close(fd);
        return PTR_ERR(poll);
    }
    fd->epollfd.poll = poll;
    return f_install(fd, flags);
}
fd_t sys_epoll_create0() {
    return sys_epoll_create(0);
}

struct epoll_event_ {
    uint32_t events;
#ifdef GUEST_ARM64
    uint32_t __pad;
#endif
    uint64_t data;
}
#ifndef GUEST_ARM64
__attribute__((packed))
#endif
;

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3
#define EPOLLET_ (1 << 31)
#define EPOLLONESHOT_ (1 << 30)

int_t sys_epoll_ctl(fd_t epoll_f, int_t op, fd_t f, addr_t event_addr) {
    STRACE("epoll_ctl(%d, %d, %d, %#x)", epoll_f, op, f, event_addr);
    struct fd *epoll = f_get_retained(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    struct fd *fd = NULL;
    int res = _EINVAL;
    if (epoll->ops != &epoll_ops)
        goto out;
    fd = f_get_retained(f);
    if (fd == NULL) { res = _EBADF; goto out; }
    if (fd == epoll)
        goto out;
    // Keep regular-file readiness in poll/select, not guest epoll.
    if (op == EPOLL_CTL_ADD_ && (S_ISREG(fd->type) || S_ISDIR(fd->type))) {
        res = _EPERM;
        goto out;
    }
    if (op == EPOLL_CTL_DEL_) {
        res = poll_del_fd(epoll->epollfd.poll, fd, f);
        goto out;
    }
    if (op != EPOLL_CTL_ADD_ && op != EPOLL_CTL_MOD_)
        goto out;
    struct epoll_event_ event;
    if (user_get(event_addr, event)) { res = _EFAULT; goto out; }
    STRACE(" {events: %#x, data: %#x}", event.events, event.data);
    // Duplicate detection is atomic with insertion under poll->lock.
    res = op == EPOLL_CTL_ADD_
        ? poll_add_fd(epoll->epollfd.poll, fd, f, event.events, (union poll_fd_info) event.data)
        : poll_mod_fd(epoll->epollfd.poll, fd, f, event.events, (union poll_fd_info) event.data);
out:
    if (fd) fd_close(fd);
    fd_close(epoll);
    return res;
}

struct epoll_context {
    struct epoll_event_ *events;
    int n;
    int max_events;
};

static int epoll_callback(void *context, int types, union poll_fd_info info) {
    struct epoll_context *c = context;
    if (c->n >= c->max_events)
        return 0;
    c->events[c->n++] = (struct epoll_event_) {.events = types, .data = info.num};
    return 1;
}

int_t sys_epoll_wait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout) {
    STRACE("epoll_wait(%d, %#x, %d, %d)", epoll_f, events_addr, max_events, timeout);
    struct fd *epoll = f_get_retained(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops || max_events <= 0 ||
            max_events > INT_MAX / (int) sizeof(struct epoll_event_)) {
        fd_close(epoll);
        return _EINVAL;
    }
    struct epoll_event_ *events = malloc((size_t) max_events * sizeof(*events));
    if (!events) { fd_close(epoll); return _ENOMEM; }
    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    struct epoll_context context = {.events = events, .n = 0, .max_events = max_events};
    STRACE("...\n");
    int res = poll_wait(epoll->epollfd.poll, epoll_callback, &context, timeout < 0 ? NULL : &timeout_ts);
    STRACE("%d end epoll_wait", current->pid);
    if (res >= 0) {
        for (int i = 0; i < res; i++) {
            STRACE(" {events: %#x, data: %#x}", events[i].events, events[i].data);
        }
        if (user_write(events_addr, events, sizeof(struct epoll_event_) * res))
            res = _EFAULT;
    } else if (res != _EINTR) {
        // Host-level errors from kqueue/kevent should not propagate to the guest
        // as unexpected errnos. libuv asserts that epoll_pwait only fails with
        // EINTR; anything else (EBADF from kqueue race, etc.) crashes the guest.
        // Convert to EINTR so the guest's event loop retries gracefully.
        printk("epoll_wait: converting error %d to EINTR (pid=%d)\n", res, current->pid);
        res = _EINTR;
    }
    free(events);
    fd_close(epoll);
    return res;
}

int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout, addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        int err = user_get_sigset(sigmask_addr, sigsetsize, &mask);
        if (err)
            return err;
        sigmask_set_temp(mask);
    }

    return sys_epoll_wait(epoll_f, events_addr, max_events, timeout);
}

static int epoll_close(struct fd *fd) {
    if (fd->epollfd.poll)
        poll_destroy(fd->epollfd.poll);
    return 0;
}

static struct fd_ops epoll_ops = {
    .close = epoll_close,
};

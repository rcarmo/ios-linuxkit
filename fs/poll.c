#include "kernel/task.h"
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <sched.h>
#include "misc.h"
#include "util/list.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"

#include "fs/sockrestart.h"


#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);

/// Safe close that skips fds 0-2 (stdin/stdout/stderr).
/// iOS guards these descriptors — closing them triggers EXC_GUARD and kills the app.
/// The iSH kernel operates on host fds that should never be 0/1/2, but race
/// conditions during fd reuse can occasionally produce them.
static inline void safe_close(int fd) {
    if (fd > 2)
        close(fd);
}

// lock order: fd, then poll

struct poll *poll_create() {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0) {
        int saved = errno_map();
        free(poll);
        return ERR_PTR(saved);
    }
    poll->waiters = 0;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    lock_init(&poll->lock);
    return poll;
}

static inline bool poll_fd_is_real(struct poll_fd *pollfd) {
    // poll/select accept regular files and directories as immediately ready,
    // but the host epoll backend rejects their registration with EPERM. Keep
    // them in the userspace readiness scan instead. Guest epoll_ctl has its
    // own explicit regular-file/directory rejection and remains unchanged.
    mode_t_ type = pollfd->fd->type;
    return pollfd->fd->ops->poll == realfs_poll && type != S_IFREG && type != S_IFDIR;
}

// does not do its own locking
// Match on the (struct fd, guest fd number) pair — see fd_no in fs/poll.h.
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd, int fd_no) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd && poll_fd->fd_no == fd_no)
            return poll_fd;
    }
    return NULL;
}

static int poll_real_types(struct poll *poll, struct fd *fd) {
    int types = 0;
    struct poll_fd *pf;
    list_for_each_entry(&poll->poll_fds, pf, fds)
        if (pf->fd == fd && !pf->oneshot_fired)
            types |= pf->types;
    return types & ~POLL_ONESHOT;
}

// Recompute the host (real) registration for `fd` in this poll as the UNION
// of the event types of every registration referring to it. Several guest
// registrations (different fd numbers, same description after dup) share one
// host registration, since the host backend is keyed by real_fd. Passing 0
// types deletes the host registration. poll->lock must be held.
static int poll_real_refresh(struct poll *poll, struct fd *fd) {
    int types = 0;
    struct poll_fd *pf, *first = NULL;
    list_for_each_entry(&poll->poll_fds, pf, fds) {
        if (pf->fd == fd) {
            if (first == NULL)
                first = pf;
            if (!pf->oneshot_fired)
                types |= pf->types;
        }
    }
    return real_poll_update(&poll->real, fd->real_fd, types, first);
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

bool poll_has_fd(struct poll *poll, struct fd *fd, int fd_no) {
    // OpenMinis/ish-arm64 7710898f: membership traversal must serialize
    // with deletion. This does not itself retain the caller's fd objects.
    lock(&poll->lock);
    bool found = poll_find_fd(poll, fd, fd_no) != NULL;
    unlock(&poll->lock);
    return found;
}

int poll_add_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);

    if (poll_find_fd(poll, fd, fd_no)) { err = _EEXIST; goto out; }
    struct poll_fd *poll_fd;
    // A sleeping waiter may still hold a returned backend pointer. Reusing
    // its retired storage before all waiters leave would turn stale events
    // into events for an unrelated new registration (ABA).
    if (!poll->waiters && !list_empty(&poll->pollfd_freelist)) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->fd_no = fd_no;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;
    poll_fd->oneshot_fired = false;

    // Link first, then refresh the (shared) host registration to the union of
    // all registrations of this description in this poll.
    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            poll_fd_free(poll_fd);
            err = errno_map();
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_del_fd(struct poll *poll, struct fd *fd, int fd_no) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    bool is_real = poll_fd_is_real(poll_fd);
    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);

    // Refresh (or delete, if this was the last registration) the shared host
    // registration to the union of the remaining registrations.
    if (is_real) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    int old_types = poll_fd_is_real(poll_fd) ? poll_real_types(poll, fd) : 0;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0; // MOD re-arms even an already-ready ET fd
    poll_fd->oneshot_fired = false; // EPOLL_CTL_MOD re-arms a fired oneshot

    if (poll_fd_is_real(poll_fd) && old_types != poll_real_types(poll, fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    // MOD re-arms the userspace readiness state even when the shared host
    // union is unchanged. Wake a waiter already past its readiness scan via
    // the notify pipe, NOT a host MOD (which would duplicate a ready dup's
    // edge). The poll lock pins the nonblocking pipe; EAGAIN means a wake is
    // already queued. Notifying every successful MOD also covers emulated fds.
    if (poll->notify_pipe[1] != -1) {
        ssize_t written;
        do {
            written = write(poll->notify_pipe[1], "", 1);
        } while (written < 0 && errno == EINTR);
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        lock(&poll_fd->poll->lock);
        if (poll_fd_is_real(poll_fd))
            real_poll_update(&poll_fd->poll->real, fd->real_fd, 0, poll_fd);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        struct poll *poll = poll_fd->poll;
        poll_fd_free(poll_fd);
        unlock(&poll->lock);
    }
    unlock(&fd->poll_lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock);
        if (poll_fd->oneshot_fired) {
            unlock(&poll->lock);
            continue;
        }
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        if (poll->notify_pipe[1] != -1)
            write(poll->notify_pipe[1], "", 1);
        unlock(&poll->lock);
        // oneshot?
    }
    unlock(&fd->poll_lock);
}

static uint64_t poll_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000000000ULL + now.tv_nsec;
}

// Never take pids_lock while holding the poll lock: exit can close fds.
// The exit safety valve may orphan this thread and reap its group while a
// host wait or readiness callback runs. A pre-check alone cannot pin a group.
static int poll_task_status(bool idle_check, bool *pending) {
    int res = 0;
    lock(&pids_lock);
    if (current->exiting || current->group == NULL) { res = _EINTR; goto out; }
    struct tgroup *group = current->group;
    if (group->doing_group_exit) { res = _EINTR; goto out; }
    if (pending) {
        *pending = false;
        if (current->sighand) {
            lock(&current->sighand->lock);
            *pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
        }
    }
    if (idle_check && current->pid != 1 && group->tty == NULL) {
        uint64_t now = poll_now_ns();
        uint64_t last = atomic_load_explicit(&group->last_progress_ns, memory_order_relaxed);
        int64_t idle_s = (int64_t)(now - last) / 1000000000LL;
        if (idle_s >= 60) {
            bool children = false;
            unsigned threads = 0, nonblocking = 0;
            lock(&group->lock);
            // Do not form an over-aligned task pointer from the list-head
            // sentinel: UBSan rightly diagnoses that container_of expression.
            struct list *link;
            list_for_each(&group->threads, link) {
                struct task *t = list_entry(link, struct task, group_links);
                threads++;
                if (!t->blocking) nonblocking++;
                struct list *child_link;
                list_for_each(&t->children, child_link) {
                    struct task *child = list_entry(child_link, struct task, siblings);
                    if (child->group != group && !child->zombie) children = true;
                }
            }
            unlock(&group->lock);
            if (!children && !nonblocking) {
                printk("SAFETY-VALVE[poll]: pid=%d idle %llds, %u threads -> SIGKILL\n",
                       current->pid, (long long)idle_s, threads);
                // Defer exit until the syscall has released its retained fds
                // and notify pipe. Never exit with poll/pids locks held.
                send_signal(current, SIGKILL_, SIGINFO_NIL);
                res = _EINTR;
            }
        }
    }
out:
    unlock(&pids_lock);
    return res;
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    uint64_t deadline = UINT64_MAX;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000)
            return _EINVAL;
        uint64_t now = poll_now_ns();
        if ((uint64_t) timeout->tv_sec <= (UINT64_MAX - now - timeout->tv_nsec) / 1000000000ULL)
            deadline = now + (uint64_t) timeout->tv_sec * 1000000000ULL + timeout->tv_nsec;
    }
    lock(&poll_->lock);

    // acquire the pipe
    if (poll_->waiters++ == 0) {
        assert(poll_->notify_pipe[0] == -1 && poll_->notify_pipe[1] == -1);
        if (pipe(poll_->notify_pipe) < 0) {
            int saved = errno_map();
            poll_->waiters--;
            unlock(&poll_->lock);
            return saved;
        }
        fcntl(poll_->notify_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(poll_->notify_pipe[1], F_SETFL, O_NONBLOCK);
        if (real_poll_update(&poll_->real, poll_->notify_pipe[0], POLL_READ, NULL) < 0) {
            int saved = errno_map();
            safe_close(poll_->notify_pipe[0]); safe_close(poll_->notify_pipe[1]);
            poll_->notify_pipe[0] = poll_->notify_pipe[1] = -1;
            poll_->waiters--;
            unlock(&poll_->lock);
            return saved;
        }
    }

    int res = 0;
    unsigned spurious = 0;
    // Consume queued host edges BEFORE delivering the first readiness scan.
    // Otherwise the first queued edge survives that delivery and looks like a
    // second edge while the fd is still ready. Bound the harvest by a snapshot
    // of registrations (plus notify); level-ready/spurious wakes cannot spin.
    unsigned registrations = 1;
    bool harvesting = false, waited = false, have_real = false;
    struct poll_fd *member;
    list_for_each_entry(&poll_->poll_fds, member, fds) {
        registrations++;
        if (poll_fd_is_real(member)) {
            have_real = true;
            if (member->types & POLL_EDGETRIGGERED)
                harvesting = true;
        }
    }
    unsigned harvest_left = (registrations + 3) / 4;
    while (true) {
        bool signal_pending = false;
        unlock(&poll_->lock);
        int task_status = poll_task_status(false, &signal_pending);
        lock(&poll_->lock);
        if (task_status) { res = task_status; break; }
        if (harvesting)
            goto wait_backend;
        // check if any fds are ready
        struct poll_fd *poll_fd, *tmp;
        list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
            if (poll_fd->oneshot_fired)
                continue;
            struct fd *fd = poll_fd->fd;
            int poll_types = 0;
            if (fd->ops->poll)
                poll_types = fd->ops->poll(fd);
            poll_types &= poll_fd->types | POLL_HUP | POLL_ERR;
            if (poll_fd->types & POLL_EDGETRIGGERED) {
                poll_types &= ~poll_fd->triggered_types;
            }
            if (poll_types) {
                if (callback(context, poll_types, poll_fd->info) != 1)
                    continue; // A full result buffer must not consume an edge.
                res++;

                // The real poll does not actually get the FDs set as oneshot.
                // But this loop is done while holding the lock, so only one
                // thread can get each oneshot event. This doesn't solve the
                // thundering herd problem at all, but at least the semantics
                // are right. I'll just leave that as a TODO.
                if (poll_fd->types & POLL_ONESHOT) {
                    // Disable in place; do NOT unlink. We hold only
                    // poll->lock here, but poll_fd->polls is walked by
                    // poll_wakeup under fd->poll_lock — unlinking/freeing
                    // raced it and crashed the tty input thread (SIGSEGV in
                    // poll_wakeup when bun registers stdin with
                    // EPOLLONESHOT). Linux semantics also keep a fired
                    // oneshot registered, disabled until EPOLL_CTL_MOD.
                    bool is_real = poll_fd_is_real(poll_fd);
                    int old_types = is_real ? poll_real_types(poll_, fd) : 0;
                    poll_fd->oneshot_fired = true;
                    if (is_real && old_types != poll_real_types(poll_, fd)) {
                        // Refresh the shared host registration to the union
                        // of the still-armed registrations. An unchanged
                        // union must NOT issue MOD: that queues a stale edge
                        // for still-ready, still-armed dup registrations.
                        poll_real_refresh(poll_, fd);
                    }
                }

                if (poll_fd->types & POLL_EDGETRIGGERED) {
                    poll_fd->triggered_types |= poll_types;
                }
            }
        }
        if (res > 0) {
            uint64_t now = poll_now_ns();
            current->last_unblocked_ns = now;
            break;
        }

        if (signal_pending) {
            res = _EINTR;
            break;
        }

        // Charge all elapsed time, including rejected events and retries.
        if (timeout && poll_now_ns() >= deadline) {
            if (waited || !have_real)
                break;
            // Even timeout zero must harvest once, then re-scan readiness.
            harvesting = true;
        }

wait_backend:;
        // wait for a ready notification
        unsigned listen_count = current->sockrestart.count;
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        listen_count = current->sockrestart.count - listen_count;
        unlock(&poll_->lock);
        int err;
        int saved_errno;
        struct real_poll_event e[4];
        // Re-check signals at least once per second, but never restart the
        // caller's relative timeout after an event or sockrestart retry.
        struct timespec bounded_timeout;
        struct timespec *wait_timeout = &bounded_timeout;
        current->blocking = true;
        if (!harvesting && spurious >= 64) {
            uint64_t now = poll_now_ns();
            uint64_t ns = timeout ? (deadline > now ? deadline - now : 0) : 1000000;
            if (ns > 1000000) ns = 1000000;
            struct timespec pause = {.tv_nsec = ns};
            nanosleep(&pause, NULL); // unlocked; at most 1ms and deadline-bounded
        }
        bool restart;
        do {
            uint64_t now = poll_now_ns();
            uint64_t ns = harvesting ? 0 : timeout ? (deadline > now ? deadline - now : 0) : 1000000000ULL;
            if (ns > 1000000000ULL) ns = 1000000000ULL;
            bounded_timeout = (struct timespec) {.tv_sec = ns / 1000000000ULL, .tv_nsec = ns % 1000000000ULL};
            err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout);
            saved_errno = errno;
            restart = err < 0 && saved_errno == EINTR && sockrestart_should_restart_listen_wait();
            if (restart && (harvesting || (timeout && poll_now_ns() >= deadline))) {
                // Internal restart at expiry is not a guest signal. Re-scan
                // readiness/signals once, then return the ordinary timeout.
                err = 0;
                break;
            }
        } while (restart && !(current->exiting || current->group == NULL));
        current->blocking = false;
        // Balance the snapshot, not the possibly modified member list. A
        // socket can be removed/final-closed while the backend is sleeping.
        sockrestart_end_listen_wait_count(listen_count);
        task_status = poll_task_status(err == 0, NULL);
        lock(&poll_->lock);
        if (task_status) { res = task_status; break; }
        waited = true;
        if (harvesting && (--harvest_left == 0 || err < 4))
            harvesting = false;
        // Host wakeups alone do not count as guest progress.
        if (err > 0) { if (spurious < 64) spurious++; }
        else spurious = 0;
        if (err == 0) continue; // next scan honours the absolute deadline

        if (err < 0) {
            errno = saved_errno;
            res = errno_map();
            break;
        }

        // One host registration represents ALL guest dup registrations.
        for (int i = 0; i < err; i++) {
            struct poll_fd *triggered_poll_fd = rpe_data(&e[i]);
            if (triggered_poll_fd != NULL && triggered_poll_fd->poll == poll_) {
                struct fd *fd = triggered_poll_fd->fd;
                list_for_each_entry(&poll_->poll_fds, member, fds) {
                    if (member->fd == fd && !member->oneshot_fired &&
                            member->types & POLL_EDGETRIGGERED)
                        member->triggered_types &= ~rpe_events(&e[i]);
                }
            }
        }

        char notify_byte;
        if (read(poll_->notify_pipe[0], &notify_byte, 1) < 0 && errno != EAGAIN) {
            res = errno_map();
            break;
        }
    }

    // release the pipe
    if (--poll_->waiters == 0) {
        real_poll_update(&poll_->real, poll_->notify_pipe[0], 0, NULL);
        safe_close(poll_->notify_pipe[0]);
        safe_close(poll_->notify_pipe[1]);
        poll_->notify_pipe[0] = -1;
        poll_->notify_pipe[1] = -1;
    }

    unlock(&poll_->lock);
    return res;
}

void poll_destroy(struct poll *poll) {
    struct poll_fd *poll_fd;
    struct poll_fd *tmp;
    lock(&poll->lock);
    assert(poll->waiters == 0);
restart:
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        // Cleanup locks fd then poll. Never block in the opposite order.
        if (trylock(&poll_fd->fd->poll_lock) != 0) {
            unlock(&poll->lock);
            sched_yield();
            lock(&poll->lock);
            goto restart;
        }
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->fd->poll_lock);
        free(poll_fd);
    }

    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    real_poll_close(&poll->real);
    unlock(&poll->lock);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL)
        timeout_millis = timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    types &= ~EPOLLONESHOT;
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
        e[0].flags |= EV_CLEAR; // full pipes may ignore NOTE_LOWAT; contain rejected wakes
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        if (types & POLL_EDGETRIGGERED)
            e[i].flags |= EV_CLEAR;
    }

    return kevent(real->fd, e, 3, e, 3, NULL);
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}
static int rpe_events(struct real_poll_event *rpe) {
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) return POLL_ERR;
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    safe_close(real->fd);
}


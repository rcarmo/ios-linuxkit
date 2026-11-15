// Real poll implementation, virtual monotonic time and injected host wakes.
// The clock is enabled only around the call under test; no wall-time oracle.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "fs/sockrestart.h"

static int injected_wait_for(cond_t *, lock_t *, struct timespec *);
// Include actual do_wait so its sleep boundary is controlled, not duplicated.
#define wait_for injected_wait_for
#include "kernel/exit.c"
#undef wait_for

static struct poll *poller;
static struct tgroup *saved_group;
static void *dead_group;
static bool virtual_time;
static uint64_t now_ns, advance_ns, requested_ns;
static unsigned calls, sleeps, begins, ends, callbacks;
static int wake_kind, wake_result, orphan_kind;
static int pipe_fds[2];
static bool kill_probe;
static unsigned kill_requests;
void __real_send_signal(struct task *, int, struct siginfo_);
void __wrap_send_signal(struct task *target, int sig, struct siginfo_ info) {
    if (!kill_probe) { __real_send_signal(target, sig, info); return; }
    assert(target == current && sig == SIGKILL_ && ++kill_requests == 1);
    assert(pthread_mutex_trylock(&pids_lock.m) == EBUSY);
    assert(!pthread_mutex_trylock(&poller->lock.m));
    assert(!pthread_mutex_unlock(&poller->lock.m));
    assert(!current->blocking && !current->sockrestart.count && list_empty(&current->sockrestart.listen));
    assert(poller->waiters == 1 && poller->notify_pipe[0] >= 0 && poller->notify_pipe[1] >= 0);
    // Capture the deferred guest kill request without delivering it to this
    // host-linked fixture. poll_wait must return and release the pipe first.
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    if (virtual_time && (m == &pids_lock.m || (current->sighand && m == &current->sighand->lock.m))) {
        assert(!pthread_mutex_trylock(&poller->lock.m));
        assert(!pthread_mutex_unlock(&poller->lock.m));
        if (m != &pids_lock.m) assert(pthread_mutex_trylock(&pids_lock.m) == EBUSY);
    }
    return __real_pthread_mutex_lock(m);
}
struct deadline_slot {
    pthread_barrier_t entered, resume;
    struct task task;
    uint64_t now, duration;
    unsigned calls, begins, ends;
    bool removed;
    int pipe[2];
};
static _Thread_local struct deadline_slot *deadline_slot;
static void barrier(pthread_barrier_t *b) {
    int r = pthread_barrier_wait(b); assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *ts) {
    if (deadline_slot) {
        assert(id == CLOCK_MONOTONIC);
        *ts = (struct timespec){.tv_sec = deadline_slot->now / 1000000000ULL,
            .tv_nsec = deadline_slot->now % 1000000000ULL}; return 0;
    }
    if (!virtual_time) return __real_clock_gettime(id, ts);
    assert(id == CLOCK_MONOTONIC);
    *ts = (struct timespec){.tv_sec = now_ns / 1000000000ULL, .tv_nsec = now_ns % 1000000000ULL}; return 0;
}
int __real_nanosleep(const struct timespec *, struct timespec *);
int __wrap_nanosleep(const struct timespec *ts, struct timespec *remaining) {
    if (!virtual_time) return __real_nanosleep(ts, remaining);
    uint64_t ns = (uint64_t)ts->tv_sec * 1000000000ULL + ts->tv_nsec;
    assert(ts->tv_sec == 0 && ns > 0 && ns <= 1000000 && calls >= 64);
    assert(!pthread_mutex_trylock(&poller->lock.m)); assert(!pthread_mutex_unlock(&poller->lock.m));
    if (requested_ns) assert(now_ns + ns <= 10000000000ULL + requested_ns);
    sleeps++; now_ns += ns; return 0;
}
static void orphan(void) {
    if (orphan_kind == 1) current->group = NULL;
    else { current->exiting = true; current->group = dead_group; }
}
int __real_epoll_wait(int, struct epoll_event *, int, int);
int __wrap_epoll_wait(int fd, struct epoll_event *events, int max, int ms) {
    if (deadline_slot) {
        struct deadline_slot *s = deadline_slot;
        assert(fd == poller->real.fd && max > 0 && current->blocking);
        uint64_t left = 10000000000ULL + s->duration - s->now;
        assert(left > 0 && ms == (int)((left + 999999) / 1000000));
        assert(++s->calls <= 25);
        if (s->calls == 1) {
            s->pipe[0] = poller->notify_pipe[0]; s->pipe[1] = poller->notify_pipe[1];
            barrier(&s->entered); barrier(&s->resume);
        }
        s->now += s->removed ? left : (left < 1000000 ? left : 1000000);
        events[0] = (struct epoll_event){.events = EPOLLIN, .data.ptr = NULL};
        return 1;
    }
    if (!virtual_time) return __real_epoll_wait(fd, events, max, ms);
    assert(max > 0 && fd == poller->real.fd && poller->waiters == 1 && current->blocking);
    assert(ms >= 0 && ms <= 1000); calls++; assert(calls < 5000);
    assert(!pthread_mutex_trylock(&poller->lock.m)); assert(!pthread_mutex_unlock(&poller->lock.m));
    uint64_t left = requested_ns ? 10000000000ULL + requested_ns - now_ns : (ms == 0 ? 0 : 1000000000ULL);
    if (left > 1000000000ULL) left = 1000000000ULL;
    assert(ms == (int)((left + 999999) / 1000000)); // remaining deadline, never restarted
    pipe_fds[0] = poller->notify_pipe[0]; pipe_fds[1] = poller->notify_pipe[1];
    if (wake_kind == 1) current->pending = 1;
    if (wake_kind == 2) orphan();
    if (wake_kind == 3 || wake_kind == 4) current->sockrestart.punt = true;
    if (wake_kind == 4 && now_ns + advance_ns >= 10000000000ULL + requested_ns)
        current->pending = 1;
    now_ns += advance_ns < left ? advance_ns : left;
    events[0] = (struct epoll_event){.events = EPOLLIN, .data.ptr = NULL};
    if (wake_result < 0) errno = EINTR;
    return wake_result;
}
void __real_sockrestart_begin_listen_wait(struct fd *);
void __wrap_sockrestart_begin_listen_wait(struct fd *fd) {
    if (deadline_slot) deadline_slot->begins++; else if (virtual_time) begins++;
    __real_sockrestart_begin_listen_wait(fd);
}
void __real_sockrestart_end_listen_wait_count(unsigned);
void __wrap_sockrestart_end_listen_wait_count(unsigned count) {
    assert(!current->blocking);
    if (deadline_slot) deadline_slot->ends += count; else if (virtual_time) ends += count;
    __real_sockrestart_end_listen_wait_count(count);
}
static int callback(void *ctx, int types, union poll_fd_info info) { (void)ctx; (void)types; (void)info; callbacks++; return 1; }
static void reset(void) {
    now_ns = 10000000000ULL; calls = sleeps = begins = ends = callbacks = 0;
    pipe_fds[0] = pipe_fds[1] = -1; virtual_time = true;
    wake_kind = 0; wake_result = 1; orphan_kind = 0;
}
static void clean(void) {
    virtual_time = false;
    current->group = saved_group; current->exiting = false; current->pending = 0; current->sockrestart.punt = false;
    assert(!current->blocking && poller->waiters == 0 && begins == ends && callbacks == 0);
    assert(current->sockrestart.count == 0 && list_empty(&current->sockrestart.listen));
    assert(poller->notify_pipe[0] == -1 && poller->notify_pipe[1] == -1);
    for (int i = 0; i < 2; i++) if (pipe_fds[i] >= 0) assert(fcntl(pipe_fds[i], F_GETFD) == -1 && errno == EBADF);
    assert(!trylock(&poller->lock)); unlock(&poller->lock);
}
static void deadline(uint64_t ns, uint64_t step) {
    reset(); requested_ns = ns; advance_ns = step;
    struct timespec ts = {.tv_sec = ns / 1000000000ULL, .tv_nsec = ns % 1000000000ULL};
    uint64_t progress = atomic_load(&saved_group->last_progress_ns);
    assert(poll_wait(poller, callback, NULL, &ts) == 0);
    assert(now_ns == 10000000000ULL + ns && atomic_load(&saved_group->last_progress_ns) == progress);
    if (ns == 0) assert(calls == 1 && begins == 1 && sleeps == 0);
    else assert(calls >= 64 && sleeps > 0 && begins == calls);
    clean();
}
static void retry_deadline(bool pending) {
    reset(); requested_ns = 25000000; advance_ns = 1000000; wake_kind = pending ? 4 : 3; wake_result = -1;
    struct timespec ts = {.tv_nsec = 25000000};
    assert(poll_wait(poller, callback, NULL, &ts) == (pending ? _EINTR : 0));
    assert(now_ns == 10000000000ULL + requested_ns && calls == 25 && begins == 1 && ends == 1 && sleeps == 0);
    clean();
}
static void wake_cleanup(int kind, int result, int orphan_mode) {
    reset(); requested_ns = 0; advance_ns = 100000; wake_kind = kind; wake_result = result; orphan_kind = orphan_mode;
    assert(poll_wait(poller, callback, NULL, NULL) == _EINTR);
    assert(calls == 1 && begins == 1 && ends == 1); clean();
}
static void deferred_kill(void) {
    struct task *parent = current, local = *current;
    assert(list_size(&saved_group->threads) == 1);
    uint64_t progress = atomic_load(&saved_group->last_progress_ns);
    struct tty *tty = saved_group->tty;
    // Model an orphan host waiter not on the live group's thread list. Its
    // remaining live member is blocked, satisfying the existing idle valve.
    parent->blocking = true; local.pid = 2; local.blocking = false;
    local.sockrestart.count = 0; list_init(&local.sockrestart.listen); current = &local;
    saved_group->tty = NULL; atomic_store(&saved_group->last_progress_ns, 0);
    reset(); now_ns = 70000000000ULL; requested_ns = 0; advance_ns = 100000; wake_result = 0;
    kill_probe = true; kill_requests = 0;
    assert(poll_wait(poller, callback, NULL, NULL) == _EINTR);
    kill_probe = false;
    assert(kill_requests == 1 && calls == 1 && begins == 1 && ends == 1 && !local.exiting);
    clean(); current = parent; parent->blocking = false;
    saved_group->tty = tty; atomic_store(&saved_group->last_progress_ns, progress);
    assert(!trylock(&pids_lock)); unlock(&pids_lock);
}
static void *deadline_worker(void *arg) {
    struct deadline_slot *s = arg; deadline_slot = s; current = &s->task;
    current->thread = pthread_self();
    struct timespec ts = {.tv_nsec = s->duration};
    assert(poll_wait(poller, callback, NULL, &ts) == 0);
    assert(s->now == 10000000000ULL + s->duration);
    if (s->removed) assert(s->calls == 1 && s->begins == 2 && s->ends == 2);
    else assert(s->begins == s->calls && s->ends == s->calls);
    assert(!current->blocking && !current->sockrestart.count && list_empty(&current->sockrestart.listen));
    current = NULL; deadline_slot = NULL; return NULL;
}
static void simultaneous_deadlines(void) {
    struct deadline_slot slots[2] = {0}; pthread_t threads[2];
    uint64_t progress = atomic_load(&saved_group->last_progress_ns);
    assert(!virtual_time);
    for (int i = 0; i < 2; i++) {
        slots[i].task = *current; slots[i].task.blocking = false;
        list_init(&slots[i].task.sockrestart.listen); slots[i].task.sockrestart.count = 0;
        slots[i].now = 10000000000ULL; slots[i].duration = i ? 25000000 : 5000000;
        assert(!pthread_barrier_init(&slots[i].entered, NULL, 2));
        assert(!pthread_barrier_init(&slots[i].resume, NULL, 2));
        assert(!pthread_create(&threads[i], NULL, deadline_worker, &slots[i]));
        barrier(&slots[i].entered);
    }
    assert(poller->waiters == 2);
    for (int i = 0; i < 2; i++) assert(slots[0].pipe[i] == slots[1].pipe[i]);
    barrier(&slots[0].resume); assert(!pthread_join(threads[0], NULL));
    assert(slots[0].calls == 5 && slots[1].calls == 1 && poller->waiters == 1);
    for (int i = 0; i < 2; i++)
        assert(poller->notify_pipe[i] == slots[1].pipe[i] && fcntl(slots[1].pipe[i], F_GETFD) >= 0);
    // First deadline expired without closing the second waiter's shared pipe
    // or restarting its independently charged 25ms relative deadline.
    barrier(&slots[1].resume); assert(!pthread_join(threads[1], NULL));
    assert(slots[1].calls == 25 && poller->waiters == 0 && !callbacks);
    assert(atomic_load(&saved_group->last_progress_ns) == progress);
    for (int i = 0; i < 2; i++) {
        assert(poller->notify_pipe[i] == -1);
        assert(fcntl(slots[1].pipe[i], F_GETFD) == -1 && errno == EBADF);
        assert(!pthread_barrier_destroy(&slots[i].entered) && !pthread_barrier_destroy(&slots[i].resume));
    }
    assert(!trylock(&poller->lock)); unlock(&poller->lock);
}
static void socket_member_cleanup(bool final_close) {
    const addr_t pair_addr = 0x10000;
    assert(!sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, pair_addr));
    int pair[2]; assert(!user_get(pair_addr, pair));
    struct fd *fd = f_get(pair[0]); assert(fd && atomic_load(&fd->refcount) == 1);
    int host_fd = fd->real_fd;
    poller = poll_create(); assert(!IS_ERR(poller));
    // Two registrations ensure the end API balances the captured count, not
    // one hook invocation or a traversal of the now-empty membership list.
    for (int n = 7; n <= 8; n++)
        assert(!poll_add_fd(poller, fd, n, POLL_READ, (union poll_fd_info){0}));
    struct poll_fd *retired[2];
    retired[0] = list_first_entry(&poller->poll_fds, struct poll_fd, fds);
    retired[1] = list_next_entry(retired[0], fds);
    struct deadline_slot s = {.task = *current, .now = 10000000000ULL,
        .duration = 5000000, .removed = true};
    s.task.blocking = false; s.task.sockrestart.count = 0;
    list_init(&s.task.sockrestart.listen);
    assert(!pthread_barrier_init(&s.entered, NULL, 2));
    assert(!pthread_barrier_init(&s.resume, NULL, 2));
    pthread_t t; assert(!pthread_create(&t, NULL, deadline_worker, &s));
    barrier(&s.entered); // backend suspended with poll lock dropped
    assert(poller->waiters == 1 && s.task.blocking && s.task.sockrestart.count == 2);
    assert(!list_empty(&s.task.sockrestart.listen));
    if (final_close) {
        assert(!f_close(pair[0]) && !f_get(pair[0]));
        assert(fcntl(host_fd, F_GETFD) == -1 && errno == EBADF);
        // fd is freed: do not dereference it, even to balance listener hooks.
    } else {
        assert(!poll_del_fd(poller, fd, 7) && !poll_del_fd(poller, fd, 8));
        assert(list_empty(&fd->poll_fds) && atomic_load(&fd->refcount) == 1);
    }
    assert(list_empty(&poller->poll_fds) && list_size(&poller->pollfd_freelist) == 2);
    assert(!retired[0]->poll && !retired[1]->poll && s.task.sockrestart.count == 2);
    barrier(&s.resume); assert(!pthread_join(t, NULL));
    assert(!s.task.sockrestart.count && list_empty(&s.task.sockrestart.listen));
    assert(!poller->waiters && !callbacks);
    for (int i = 0; i < 2; i++) {
        assert(poller->notify_pipe[i] == -1);
        assert(fcntl(s.pipe[i], F_GETFD) == -1 && errno == EBADF);
    }
    assert(!trylock(&poller->lock)); unlock(&poller->lock);
    poll_destroy(poller);
    if (!final_close) assert(!f_close(pair[0]));
    assert(!f_close(pair[1]));
    assert(!pthread_barrier_destroy(&s.entered) && !pthread_barrier_destroy(&s.resume));
}
static bool wait_injection;
static int wait_result;
static unsigned wait_calls;
static int injected_wait_for(cond_t *cond, lock_t *held, struct timespec *ts) {
    if (!wait_injection) return wait_for(cond, held, ts);
    assert(cond == &saved_group->child_exit && held == &pids_lock && ts->tv_sec == 1);
    assert(current->blocking && ++wait_calls == 1);
    // wait_for returns with pids_lock held. Keep that contract even when the
    // task loses its group while asleep, just as a leaked real waiter would.
    assert(trylock(held) == EBUSY);
    if (orphan_kind) orphan();
    return wait_result;
}
static void waitpid_cleanup(int result, int mode, pid_t_ child) {
    wait_injection = true; wait_calls = 0; wait_result = result; orphan_kind = mode;
    struct siginfo_ info = {0}; uint64_t progress = atomic_load(&saved_group->last_progress_ns);
    assert(do_wait(1, child, &info, NULL, 4) == _EINTR);
    wait_injection = false; current->group = saved_group; current->exiting = false;
    assert(wait_calls == 1 && !current->blocking);
    if (mode) assert(atomic_load(&saved_group->last_progress_ns) == progress);
    assert(!trylock(&pids_lock)); unlock(&pids_lock);
}
int main(void) {
    alarm(15); assert(!mount_root(&realfs, "/") && !become_first_process()); saved_group = current->group;
    dead_group = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(dead_group != MAP_FAILED);
    // An unread real socketpair also exercises sockrestart's actual listener
    // task list/count, not just calls to its begin/end hooks.
    const addr_t pair_addr = 0x10000;
    write_wrlock(&current->mem->lock);
    assert(!pt_map_nothing(current->mem, pair_addr >> PAGE_BITS, 1, P_READ | P_WRITE));
    write_wrunlock(&current->mem->lock);
    assert(!sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, pair_addr));
    int pair[2]; assert(!user_get(pair_addr, pair)); struct fd *fd = f_get(pair[0]); assert(fd);
    poller = poll_create(); assert(!IS_ERR(poller));
    assert(!poll_add_fd(poller, fd, pair[0], POLL_READ, (union poll_fd_info){0}));
    deadline(0, 100000); deadline(25000000, 100000); deadline(1100000000ULL, 10000000); retry_deadline(false); retry_deadline(true);
    struct timespec invalid[] = {{.tv_sec = -1}, {.tv_nsec = -1}, {.tv_nsec = 1000000000}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) assert(poll_wait(poller, callback, NULL, &invalid[i]) == _EINVAL);
    for (int result = -1; result <= 1; result++) {
        wake_cleanup(1, result, 0); // guest signal after EINTR/timeout/rejected wake
        wake_cleanup(2, result, 1); // NULL group after every backend result
        wake_cleanup(2, result, 2); // exiting task with inaccessible dead group
    }
    deferred_kill(); simultaneous_deadlines();
    poll_destroy(poller); assert(!f_close(pair[0]) && !f_close(pair[1]));
    socket_member_cleanup(false); socket_member_cleanup(true);
    struct task *parent = current; assert(!become_new_init_child()); pid_t_ child = current->pid; current = parent;
    waitpid_cleanup(_EINTR, 0, child);
    for (int mode = 1; mode <= 2; mode++) {
        waitpid_cleanup(0, mode, child); waitpid_cleanup(_EINTR, mode, child); waitpid_cleanup(_ETIMEDOUT, mode, child);
    }
    assert(!munmap(dead_group, 4096));
    puts("poll-deadline-ok: zero/25ms/1100ms spurious deadline/backoff/internal retry timeout/true pending EINTR; signal/orphan cleanup/deferred SIGKILL with unlocked poll; simultaneous independent deadlines/shared pipe; socket deletion/final-close captured count/list cleanup; real do_wait orphan unlock");
}

// Deterministic host-linked probes of the candidate fd/poll/epoll code.
// Backend and mutex checkpoints arrange races; no timing/stress oracle.
#include <assert.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"

static const addr_t input = 0x10000, output = 0x10100;
struct guest_event { uint32_t events, pad; uint64_t data; };
static struct task *task;
static struct fd *ready_fd;
static int deaths;
static bool orphan_ready;
static void *dead_group;
static unsigned status_checks, orphan_callbacks;
static int readiness(struct fd *fd) {
    if (orphan_ready) {
        assert(fd == ready_fd && status_checks == 1 && !task->exiting);
        // Deterministically model reaping after the initial pinned status
        // check. The ready path must not write through this dead group.
        task->exiting = true; task->group = dead_group;
    }
    return fd == ready_fd ? POLL_READ : 0;
}
static int closed(struct fd *fd) { (void)fd; deaths++; return 0; }
static const struct fd_ops ops = {.poll = readiness, .close = closed};
static struct fd *new_fd(void) { struct fd *fd = fd_create(&ops); assert(fd); return fd; }
static void barrier(pthread_barrier_t *b) {
    int r = pthread_barrier_wait(b); assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
static pthread_barrier_t entered, resume, add_start;
static _Thread_local bool waiting, adding, destroying;
static bool gated, add_gate, copyout_fault, ctl_gate, ctl_fault, destroy_pause, forbid_wait;
static int backend_calls, watched_host = -1, host_closes;
static struct poll_fd *stale;
static struct poll *contended;
static struct fd *contended_fd, *retain_probe;
static bool check_retain, fail_alloc;
static _Atomic bool destroy_blocked;
struct multi_slot { pthread_barrier_t entered, resume; unsigned calls; };
static struct multi_slot slots[2];
static _Thread_local struct multi_slot *multi_slot;
static struct poll_fd *multi_retired;
static unsigned retired_frees;
static bool track_pipe, fail_backend, fail_pipe, watch_alloc;
static int notify_failure, pipe_calls, tracked_pipe[2], pipe_closes[2], poll_frees;
static void *poll_allocation;
int __real_pipe(int *);
int __wrap_pipe(int *fds) {
    if (fail_pipe) { fail_pipe = false; errno = EMFILE; return -1; }
    int r = __real_pipe(fds);
    if (track_pipe && !r) {
        assert(++pipe_calls == 1);
        for (int i = 0; i < 2; i++) { tracked_pipe[i] = fds[i]; pipe_closes[i] = 0; }
    }
    return r;
}
int __real_epoll_create1(int);
int __wrap_epoll_create1(int flags) {
    if (fail_backend) { fail_backend = false; errno = EMFILE; return -1; }
    return __real_epoll_create1(flags);
}
int __real_epoll_ctl(int, int, int, struct epoll_event *);
int __wrap_epoll_ctl(int ep, int op, int fd, struct epoll_event *ev) {
    if (notify_failure == op && ev && ev->data.ptr == NULL) {
        notify_failure = 0; errno = EIO; return -1;
    }
    return __real_epoll_ctl(ep, op, fd, ev);
}
int __real_epoll_wait(int, struct epoll_event *, int, int);
int __wrap_epoll_wait(int fd, struct epoll_event *e, int n, int ms) {
    assert(!forbid_wait);
    if (multi_slot) {
        assert(n > 0 && ms > 0 && ++multi_slot->calls <= 2);
        barrier(&multi_slot->entered); barrier(&multi_slot->resume);
        if (multi_slot->calls == 2) { errno = EINTR; return -1; }
        e[0] = (struct epoll_event){.events = EPOLLIN, .data.ptr = multi_retired};
        return 1;
    }
    if (!gated || !waiting) return __real_epoll_wait(fd, e, n, ms);
    assert(n > 0 && ms > 0);
    if (++backend_calls > 1) { errno = EINTR; return -1; }
    barrier(&entered); barrier(&resume);
    e[0] = (struct epoll_event){.events = EPOLLIN, .data.ptr = stale};
    return 1;
}
int __real_close(int);
int __wrap_close(int fd) {
    if (fd == watched_host) host_closes++;
    if (track_pipe) for (int i = 0; i < 2; i++)
        if (fd == tracked_pipe[i]) assert(++pipe_closes[i] == 1);
    return __real_close(fd);
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if (fail_alloc) { fail_alloc = false; return NULL; }
    void *p = __real_malloc(size);
    if (watch_alloc && size == sizeof(struct poll)) { assert(!poll_allocation); poll_allocation = p; }
    return p;
}
void __real_free(void *);
void __wrap_free(void *p) {
    if (p && p == poll_allocation) poll_frees++;
    if (p && p == multi_retired) { assert(contended->waiters == 0); retired_frees++; }
    __real_free(p);
}
int __real_user_read(addr_t, void *, size_t);
int __wrap_user_read(addr_t addr, void *buf, size_t size) {
    int r = __real_user_read(addr, buf, size);
    if (ctl_gate && waiting && addr == input) {
        assert(!r); barrier(&entered); barrier(&resume);
        if (ctl_fault) return 1;
    }
    return r;
}
int __real_user_write(addr_t, const void *, size_t);
int __wrap_user_write(addr_t addr, const void *buf, size_t size) {
    if (copyout_fault && waiting && addr == input) return 1;
    return __real_user_write(addr, buf, size);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    if (orphan_ready && (m == &pids_lock.m || m == &task->sighand->lock.m)) {
        assert(!pthread_mutex_trylock(&contended->lock.m));
        assert(!pthread_mutex_unlock(&contended->lock.m)); // poll lock dropped
        if (m == &pids_lock.m) status_checks++;
        else assert(pthread_mutex_trylock(&pids_lock.m) == EBUSY); // sighand pinned
    }
    if (adding && add_gate && m == &contended_fd->poll_lock.m) barrier(&add_start);
    return __real_pthread_mutex_lock(m);
}
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __wrap_pthread_mutex_unlock(pthread_mutex_t *m) {
    if (check_retain && m == &task->files->lock.m)
        assert(atomic_load(&retain_probe->refcount) == 2); // before publication of unlock
    int r = __real_pthread_mutex_unlock(m);
    if (destroying && destroy_pause && m == &contended->lock.m) {
        destroy_pause = false;
        barrier(&entered); barrier(&resume); // paused AFTER releasing poll lock
    }
    return r;
}
int __real_pthread_mutex_trylock(pthread_mutex_t *);
int __wrap_pthread_mutex_trylock(pthread_mutex_t *m) {
    int r = __real_pthread_mutex_trylock(m);
    if (destroying && m == &contended_fd->poll_lock.m && r == EBUSY)
        atomic_store(&destroy_blocked, true);
    return r;
}
static void start_gate(void) {
    assert(!pthread_barrier_init(&entered, NULL, 2));
    assert(!pthread_barrier_init(&resume, NULL, 2));
    gated = true; backend_calls = 0; stale = NULL;
}
static void end_gate(pthread_t t) {
    barrier(&resume); assert(!pthread_join(t, NULL)); gated = false;
    assert(!pthread_barrier_destroy(&entered)); assert(!pthread_barrier_destroy(&resume));
}
struct job { int kind, result; fd_t number, target; addr_t out; struct poll *poll; };
static int callback(void *ctx, int events, union poll_fd_info info) {
    (void)ctx; (void)events; (void)info; assert(!"unexpected guest readiness"); return 1;
}
static void *waiter(void *arg) {
    struct job *j = arg; current = task; waiting = true;
    if (j->kind == 0) j->result = sys_epoll_wait(j->number, j->out, 1, 1000);
    else if (j->kind == 1) j->result = (int)sys_poll(input, 1, 1000);
    else if (j->kind == 2) j->result = (int)sys_select(j->number + 1, input, 0, 0, 0);
    else if (j->kind == 4) j->result = sys_epoll_ctl(j->number, 1, j->target, input);
    else { struct timespec ts = {.tv_sec = 1}; j->result = poll_wait(j->poll, callback, NULL, &ts); }
    current = NULL; return NULL;
}
static void retained_lookup(void) {
    struct fd *fd = new_fd(); fd_t n = f_install(fd, 0); assert(n >= 0);
    retain_probe = fd; check_retain = true;
    struct fd *held = f_get_retained(n); check_retain = false;
    assert(held == fd); int before = deaths;
    assert(!f_close(n) && deaths == before);
    struct fd *replacement = new_fd(); assert(f_install(replacement, 0) == n);
    assert(!fd_close(held) && deaths == before + 1);
    assert(f_get(n) == replacement && atomic_load(&replacement->refcount) == 1);
    assert(!f_close(n));
    assert(!f_get_retained(-1) && !f_get_retained(INT_MAX));
    struct fdtable *files = task->files; task->files = NULL;
    assert(!f_get_retained(0)); task->files = files;
}
static void epoll_reuse(bool fault) {
    fd_t ep = sys_epoll_create(0); assert(ep >= 0);
    struct fd *old = f_get(ep), *member = new_fd();
    fd_t n = f_install(member, 0); assert(n >= 0);
    struct guest_event ev = {.events = POLL_READ, .data = 0x123456789ULL};
    assert(!user_put(input, ev) && !sys_epoll_ctl(ep, 1, n, input));
    watched_host = old->epollfd.poll->real.fd; host_closes = 0;
    start_gate(); struct job j = {.number = ep, .out = fault ? 1 : output};
    pthread_t t; assert(!pthread_create(&t, NULL, waiter, &j)); barrier(&entered);
    assert(atomic_load(&old->refcount) == 2 && old->epollfd.poll->waiters == 1);
    assert(!f_close(ep) && host_closes == 0);
    struct fd *replacement = new_fd(); assert(f_install(replacement, 0) == ep);
    assert(old != replacement && atomic_load(&old->refcount) == 1);
    ready_fd = member; end_gate(t); ready_fd = NULL;
    assert(j.result == (fault ? _EFAULT : 1) && host_closes == 1);
    assert(f_get(ep) == replacement && atomic_load(&replacement->refcount) == 1);
    if (!fault) { assert(!user_get(output, ev)); assert(ev.events == POLL_READ && ev.data == 0x123456789ULL); }
    watched_host = -1; assert(list_empty(&member->poll_fds));
    assert(!f_close(n) && !f_close(ep));
}
static void ctl_reuse(bool fault) {
    fd_t ep = sys_epoll_create(0); assert(ep >= 0);
    struct fd *old = f_get(ep), *member = new_fd(); fd_t n = f_install(member, 0); assert(n >= 0);
    struct guest_event ev = {.events = POLL_READ, .data = 42}; assert(!user_put(input, ev));
    watched_host = old->epollfd.poll->real.fd; host_closes = 0;
    start_gate(); ctl_gate = true; ctl_fault = fault;
    struct job j = {.kind = 4, .number = ep, .target = n}; pthread_t t;
    assert(!pthread_create(&t, NULL, waiter, &j)); barrier(&entered);
    assert(atomic_load(&old->refcount) == 2 && atomic_load(&member->refcount) == 2);
    int before = deaths; assert(!f_close(ep) && !f_close(n) && deaths == before && !host_closes);
    struct fd *replacement_ep = new_fd(), *replacement_fd = new_fd();
    assert(f_install(replacement_ep, 0) == ep && f_install(replacement_fd, 0) == n);
    end_gate(t); ctl_gate = false;
    assert(j.result == (fault ? _EFAULT : 0) && host_closes == 1 && deaths == before + 1);
    assert(f_get(ep) == replacement_ep && f_get(n) == replacement_fd);
    assert(atomic_load(&replacement_ep->refcount) == 1 && atomic_load(&replacement_fd->refcount) == 1);
    assert(list_empty(&replacement_ep->poll_fds) && list_empty(&replacement_fd->poll_fds));
    watched_host = -1; assert(!f_close(ep) && !f_close(n));
}
static void ctl_unwind(void) {
    fd_t ep = sys_epoll_create(0); struct fd *p = f_get(ep), *fd = new_fd();
    fd_t n = f_install(fd, 0); assert(ep >= 0 && n >= 0);
    assert(sys_epoll_ctl(ep, 1, n, 1) == _EFAULT);
    assert(sys_epoll_ctl(ep, 3, n, 1) == _EFAULT);
    assert(sys_epoll_ctl(ep, 99, n, input) == _EINVAL);
    assert(sys_epoll_ctl(ep, 1, ep, input) == _EINVAL);
    assert(sys_epoll_ctl(ep, 1, INT_MAX, input) == _EBADF);
    assert(sys_epoll_ctl(n, 1, ep, input) == _EINVAL);
    assert(sys_epoll_wait(ep, output, 0, 0) == _EINVAL);
    assert(sys_epoll_wait(ep, output, INT_MAX, 0) == _EINVAL);
    assert(sys_epoll_wait(n, output, 1, 0) == _EINVAL);
    fail_alloc = true; assert(sys_epoll_wait(ep, output, 1, 0) == _ENOMEM && !fail_alloc);
    assert(sys_epoll_ctl(ep, 2, n, 0) == _ENOENT);
    fd->type = S_IFREG; assert(sys_epoll_ctl(ep, 1, n, input) == _EPERM);
    fd->type = S_IFDIR; assert(sys_epoll_ctl(ep, 1, n, input) == _EPERM); fd->type = 0;
    assert(atomic_load(&p->refcount) == 1 && atomic_load(&fd->refcount) == 1);
    assert(list_empty(&p->epollfd.poll->poll_fds)); assert(!f_close(n) && !f_close(ep));
}
static void select_poll_reuse(int kind, bool fault) {
    struct fd *fd = new_fd(); fd_t n = f_install(fd, 0); assert(n >= 0);
    if (kind == 1) { struct pollfd_ p = {.fd = n, .events = POLL_READ}; assert(!user_put(input, p)); }
    else { char bits[32] = {0}; assert(n < 256); bit_set(n, bits); assert(!user_write(input, bits, sizeof(bits))); }
    start_gate(); struct job j = {.kind = kind, .number = n}; pthread_t t;
    assert(!pthread_create(&t, NULL, waiter, &j)); barrier(&entered);
    assert(atomic_load(&fd->refcount) == 2); int before = deaths;
    assert(!f_close(n) && deaths == before);
    struct fd *replacement = new_fd(); assert(f_install(replacement, 0) == n);
    ready_fd = fd; copyout_fault = fault; end_gate(t); ready_fd = NULL; copyout_fault = false;
    assert(j.result == (fault ? _EFAULT : 1) && deaths == before + 1 && f_get(n) == replacement);
    if (!fault && kind == 1) { struct pollfd_ p; assert(!user_get(input, p) && p.revents == POLL_READ); }
    if (!fault && kind == 2) { char bits[32]; assert(!user_read(input, bits, sizeof(bits)) && bit_test(n, bits)); }
    assert(atomic_load(&replacement->refcount) == 1 && !f_close(n));
}
static void *add_worker(void *arg) {
    int *result = arg; adding = true;
    *result = poll_add_fd(contended, contended_fd, 7, POLL_READ, (union poll_fd_info){.num = 7});
    return NULL;
}
static void atomic_add(void) {
    contended = poll_create(); assert(!IS_ERR(contended)); contended_fd = new_fd();
    assert(!pthread_barrier_init(&add_start, NULL, 2)); add_gate = true;
    int a = 99, b = 99; pthread_t x, y;
    assert(!pthread_create(&x, NULL, add_worker, &a) && !pthread_create(&y, NULL, add_worker, &b));
    assert(!pthread_join(x, NULL) && !pthread_join(y, NULL)); add_gate = false;
    assert((a == 0 && b == _EEXIST) || (b == 0 && a == _EEXIST));
    struct poll_fd *pf = list_first_entry(&contended->poll_fds, struct poll_fd, fds);
    assert(pf->fds.next == &contended->poll_fds && pf->polls.next == &contended_fd->poll_fds);
    assert(!pthread_barrier_destroy(&add_start)); poll_destroy(contended); assert(!fd_close(contended_fd));
}
static void *destroy_worker(void *unused) { (void)unused; destroying = true; poll_destroy(contended); return NULL; }
static void cleanup_destroy(void) {
    contended = poll_create(); assert(!IS_ERR(contended)); contended_fd = new_fd();
    assert(!poll_add_fd(contended, contended_fd, 7, POLL_READ, (union poll_fd_info){0}));
    // Cleanup owns fd lock before destroy takes poll lock. Destroy MUST release
    // poll lock on a failed trylock, allowing real cleanup to make progress.
    start_gate(); destroy_pause = true;
    lock(&contended_fd->poll_lock); atomic_store(&destroy_blocked, false);
    pthread_t t; assert(!pthread_create(&t, NULL, destroy_worker, NULL)); barrier(&entered);
    assert(atomic_load(&destroy_blocked)); unlock(&contended_fd->poll_lock);
    poll_cleanup_fd(contended_fd); // destroy is paused with poll lock released
    assert(list_empty(&contended_fd->poll_fds) && list_empty(&contended->poll_fds));
    end_gate(t);
    assert(!fd_close(contended_fd));
}
static void retired_nonreuse(void) {
    struct poll *p = poll_create(); assert(!IS_ERR(p)); struct fd *a = new_fd(), *b = new_fd();
    assert(!poll_add_fd(p, a, 7, POLL_READ | POLL_EDGETRIGGERED, (union poll_fd_info){.num = 7}));
    struct poll_fd *retired = list_first_entry(&p->poll_fds, struct poll_fd, fds);
    start_gate(); struct job j = {.kind = 3, .poll = p}; pthread_t t;
    assert(!pthread_create(&t, NULL, waiter, &j)); barrier(&entered);
    assert(p->waiters == 1 && !poll_del_fd(p, a, 7));
    assert(!poll_add_fd(p, b, 8, POLL_READ | POLL_EDGETRIGGERED, (union poll_fd_info){.num = 8}));
    struct poll_fd *live = list_first_entry(&p->poll_fds, struct poll_fd, fds);
    assert(live != retired && retired->poll == NULL); live->triggered_types = POLL_READ;
    stale = retired; end_gate(t);
    assert(j.result == _EINTR && backend_calls == 2 && live->triggered_types == POLL_READ);
    assert(p->waiters == 0 && p->notify_pipe[0] == -1 && p->notify_pipe[1] == -1);
    assert(!poll_add_fd(p, a, 9, POLL_READ, (union poll_fd_info){0}));
    assert(list_first_entry(&p->poll_fds, struct poll_fd, fds) == retired); // reuse legal now
    poll_destroy(p); assert(!fd_close(a) && !fd_close(b));
}
static void *multi_waiter(void *arg) {
    // Separate task-local blocking/sockrestart state; shared group/table only.
    struct task local = *task; list_init(&local.sockrestart.listen);
    local.sockrestart.count = 0; local.blocking = false; current = &local;
    multi_slot = arg; struct timespec ts = {.tv_sec = 1};
    assert(poll_wait(contended, callback, NULL, &ts) == _EINTR);
    assert(!local.blocking && !local.sockrestart.count); current = NULL; return NULL;
}
static void simultaneous_retired(void) {
    contended = poll_create(); assert(!IS_ERR(contended));
    struct fd *a = new_fd(), *b = new_fd(), *c = new_fd();
    assert(!poll_add_fd(contended, a, 7, POLL_READ | POLL_EDGETRIGGERED, (union poll_fd_info){0}));
    multi_retired = list_first_entry(&contended->poll_fds, struct poll_fd, fds); retired_frees = 0;
    track_pipe = true; pipe_calls = 0; tracked_pipe[0] = tracked_pipe[1] = -1;
    watched_host = contended->real.fd; host_closes = 0;
    pthread_t threads[2];
    for (int i = 0; i < 2; i++) {
        slots[i].calls = 0;
        assert(!pthread_barrier_init(&slots[i].entered, NULL, 2));
        assert(!pthread_barrier_init(&slots[i].resume, NULL, 2));
        assert(!pthread_create(&threads[i], NULL, multi_waiter, &slots[i]));
        barrier(&slots[i].entered);
    }
    assert(contended->waiters == 2 && pipe_calls == 1);
    assert(!poll_del_fd(contended, a, 7));
    assert(multi_retired->poll == NULL && !retired_frees);
    assert(!poll_add_fd(contended, b, 8, POLL_READ | POLL_EDGETRIGGERED, (union poll_fd_info){0}));
    struct poll_fd *live = list_first_entry(&contended->poll_fds, struct poll_fd, fds);
    assert(live != multi_retired); live->triggered_types = POLL_READ;
    barrier(&slots[0].resume); barrier(&slots[0].entered); // stale event processed; second backend call
    assert(live->triggered_types == POLL_READ && !retired_frees);
    barrier(&slots[0].resume); assert(!pthread_join(threads[0], NULL));
    assert(contended->waiters == 1 && !host_closes && !pipe_closes[0] && !pipe_closes[1]);
    for (int i = 0; i < 2; i++) assert(contended->notify_pipe[i] == tracked_pipe[i] && fcntl(tracked_pipe[i], F_GETFD) >= 0);
    assert(!poll_add_fd(contended, c, 9, POLL_READ | POLL_EDGETRIGGERED, (union poll_fd_info){0}));
    struct poll_fd *later = list_first_entry(&contended->poll_fds, struct poll_fd, fds);
    assert(later != multi_retired); later->triggered_types = POLL_READ;
    // The remaining waiter still has an already-queued pointer to the retired entry.
    barrier(&slots[1].resume); barrier(&slots[1].entered);
    assert(contended->waiters == 1 && !retired_frees);
    assert(live->triggered_types == POLL_READ && later->triggered_types == POLL_READ);
    barrier(&slots[1].resume); assert(!pthread_join(threads[1], NULL));
    assert(contended->waiters == 0 && !retired_frees && !host_closes);
    for (int i = 0; i < 2; i++) {
        assert(pipe_closes[i] == 1 && contended->notify_pipe[i] == -1);
        assert(fcntl(tracked_pipe[i], F_GETFD) == -1 && errno == EBADF);
        assert(!pthread_barrier_destroy(&slots[i].entered) && !pthread_barrier_destroy(&slots[i].resume));
    }
    track_pipe = false;
    assert(!poll_add_fd(contended, a, 10, POLL_READ, (union poll_fd_info){0}));
    assert(list_first_entry(&contended->poll_fds, struct poll_fd, fds) == multi_retired);
    poll_destroy(contended); assert(retired_frees == 1 && host_closes == 1);
    multi_retired = NULL; watched_host = -1;
    assert(!fd_close(a) && !fd_close(b) && !fd_close(c));
}
static void failure_cleanup(void) {
    watch_alloc = true; poll_allocation = NULL; poll_frees = 0; fail_backend = true;
    struct poll *p = poll_create(); assert(IS_ERR(p) && PTR_ERR(p) == _EMFILE);
    assert(!fail_backend && poll_allocation && poll_frees == 1);
    watch_alloc = false; poll_allocation = NULL;
    p = poll_create(); assert(!IS_ERR(p));
    watched_host = p->real.fd; host_closes = 0;
    struct timespec zero = {0};
    fail_pipe = true; assert(poll_wait(p, callback, NULL, &zero) == _EMFILE && !fail_pipe);
    assert(!p->waiters && p->notify_pipe[0] == -1 && p->notify_pipe[1] == -1);
    for (int op = EPOLL_CTL_ADD; op <= EPOLL_CTL_MOD; op += 2) {
        track_pipe = true; pipe_calls = 0; tracked_pipe[0] = tracked_pipe[1] = -1;
        notify_failure = op;
        assert(poll_wait(p, callback, NULL, &zero) == _EIO && !notify_failure);
        assert(pipe_calls == 1 && !p->waiters && !host_closes && !current->blocking);
        for (int i = 0; i < 2; i++) {
            assert(p->notify_pipe[i] == -1 && pipe_closes[i] == 1);
            assert(fcntl(tracked_pipe[i], F_GETFD) == -1 && errno == EBADF);
        }
        assert(!trylock(&p->lock)); unlock(&p->lock);
        // A fresh attempt must create a usable pipe, not inherit failed state.
        pipe_calls = 0; assert(poll_wait(p, callback, NULL, &zero) == 0 && pipe_calls == 1);
        assert(pipe_closes[0] == 1 && pipe_closes[1] == 1);
        track_pipe = false;
    }
    poll_destroy(p); assert(host_closes == 1); watched_host = -1;
}
static int orphan_callback(void *ctx, int events, union poll_fd_info info) {
    (void)ctx; (void)info;
    assert(events == POLL_READ && task->exiting && task->group == dead_group);
    orphan_callbacks++; return 1;
}
static void readiness_orphan(void) {
    struct tgroup *group = task->group;
    uint64_t progress = atomic_load(&group->last_progress_ns);
    dead_group = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(dead_group != MAP_FAILED);
    contended = poll_create(); assert(!IS_ERR(contended)); ready_fd = new_fd();
    assert(!poll_add_fd(contended, ready_fd, 7, POLL_READ, (union poll_fd_info){0}));
    status_checks = orphan_callbacks = 0; orphan_ready = true; forbid_wait = true;
    assert(poll_wait(contended, orphan_callback, NULL, NULL) == 1);
    orphan_ready = false; forbid_wait = false;
    assert(task->exiting && task->group == dead_group && status_checks == 1 && orphan_callbacks == 1);
    lock(&pids_lock); task->group = group; task->exiting = false; unlock(&pids_lock);
    assert(atomic_load(&group->last_progress_ns) == progress);
    assert(!task->blocking && !task->sockrestart.count && list_empty(&task->sockrestart.listen));
    assert(!contended->waiters && contended->notify_pipe[0] == -1 && contended->notify_pipe[1] == -1);
    assert(!trylock(&contended->lock)); unlock(&contended->lock);
    assert(!trylock(&pids_lock)); unlock(&pids_lock);
    assert(!trylock(&task->sighand->lock)); unlock(&task->sighand->lock);
    poll_destroy(contended); assert(list_empty(&ready_fd->poll_fds));
    assert(!fd_close(ready_fd)); ready_fd = NULL;
    assert(!munmap(dead_group, 4096)); dead_group = NULL;
}
static void guest_poll_entries(void) {
    struct fd *fd = new_fd(); fd_t n = f_install(fd, 0); assert(n >= 0);
    struct pollfd_ p[5] = {{.fd = n, .events = POLL_READ}, {.fd = n, .events = POLL_READ},
        {.fd = n, .events = POLL_WRITE}, {.fd = INT_MAX, .events = POLL_READ}, {.fd = -1, .events = POLL_READ}};
    // Infinite timeout plus an invalid entry must never enter the host wait.
    assert(!user_write(input, p, sizeof(p))); forbid_wait = true;
    assert((int)sys_poll(input, 5, -1) == 1);
    assert(!user_read(input, p, sizeof(p)) && p[3].revents == POLL_NVAL && !p[4].revents);
    ready_fd = fd; assert((int)sys_poll(input, 5, -1) == 3); ready_fd = NULL; forbid_wait = false;
    assert(!user_read(input, p, sizeof(p)) && p[0].revents == POLL_READ && p[1].revents == POLL_READ && !p[2].revents);
    assert(atomic_load(&fd->refcount) == 1 && list_empty(&fd->poll_fds)); assert(!f_close(n));
}
int main(void) {
    alarm(15); assert(!mount_root(&realfs, "/") && !become_first_process()); task = current;
    task->group->limits[RLIMIT_NOFILE_].cur = 256;
    write_wrlock(&task->mem->lock); assert(!pt_map_nothing(task->mem, input >> PAGE_BITS, 1, P_READ | P_WRITE)); write_wrunlock(&task->mem->lock);
    retained_lookup(); epoll_reuse(false); epoll_reuse(true); ctl_unwind(); ctl_reuse(false); ctl_reuse(true);
    for (int kind = 1; kind <= 2; kind++) { select_poll_reuse(kind, false); select_poll_reuse(kind, true); }
    atomic_add(); cleanup_destroy(); retired_nonreuse(); simultaneous_retired(); failure_cleanup(); guest_poll_entries(); readiness_orphan();
    for (unsigned i = 0; i < task->files->size; i++) assert(!f_get(i));
    puts("poll-lifetime-ok: retained lookup/epfd reuse/EFAULT/ctl/select/poll/atomic ADD/destroy/retired ABA/multiple waiters/shared notify/failure cleanup/guest entries/pinned status with poll lock dropped/readiness orphan PROT_NONE unwind");
}

// Host-linked guest syscalls, REAL kernel epoll/pipe/socketpair readiness.
// The wrapper counts/delegates and gates the blocking boundary; it never
// injects events or changes the kernel timeout.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"

static const addr_t input = 0x10000, output = 0x10100;
struct guest_event { uint32_t events, pad; uint64_t data; };
static _Thread_local unsigned backend_calls;
static _Thread_local bool blocking_test;
static pthread_barrier_t entered, resume;
static void barrier(pthread_barrier_t *b) {
    int r = pthread_barrier_wait(b);
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
}
int __real_epoll_wait(int, struct epoll_event *, int, int);
int __wrap_epoll_wait(int ep, struct epoll_event *events, int max, int ms) {
    assert(++backend_calls < 100);
    if (!ms) return __real_epoll_wait(ep, events, max, ms);
    assert(blocking_test && ms > 0);
    // The waiter has completed its empty readiness scan and dropped the poll
    // lock. Hold this sleep boundary until MOD finishes: the wake must survive
    // even when queued before the actual kernel sleep (no timing/sleep race).
    barrier(&entered); barrier(&resume);
    int n = __real_epoll_wait(ep, events, max, ms);
    // A timeout is a failure even if a later userspace scan finds readiness.
    // Only the notify pipe may wake us, not a synthetic duplicate host edge.
    assert(n == 1 && events[0].data.ptr == NULL && (events[0].events & EPOLLIN));
    return n;
}
static void ctl(int ep, int op, int fd, uint32_t types, uint64_t tag) {
    struct guest_event ev = {.events = types, .data = tag};
    assert(!user_put(input, ev));
    assert(!sys_epoll_ctl(ep, op, fd, input));
}
static uint64_t wait_events(int ep, int max, int expected) {
    backend_calls = 0;
    int n = sys_epoll_wait(ep, output, max, 0);
    if (n != expected)
        fprintf(stderr, "zero epoll_wait: got %d, expected %d, backend calls %u\n", n, expected, backend_calls);
    assert(n == expected);
    struct guest_event events[32];
    assert(max <= 32 && !user_read(output, events, n * sizeof(*events)));
    uint64_t tags = 0;
    for (int i = 0; i < n; i++) {
        assert(events[i].events == POLL_READ && events[i].data < 32);
        assert(!(tags & (1ULL << events[i].data)));
        tags |= 1ULL << events[i].data;
    }
    struct poll *p = f_get(ep)->epollfd.poll;
    assert(!p->waiters && p->notify_pipe[0] == -1 && p->notify_pipe[1] == -1);
    assert(!current->blocking && !current->sockrestart.count && list_empty(&current->sockrestart.listen));
    return tags;
}
static void pair(bool socket, int fds[2]) {
    assert(!(socket ? sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0, input) : sys_pipe(input)));
    assert(!user_get(input, *(int (*)[2])fds));
}
static void fill(int fd) { assert(write(f_get(fd)->real_fd, "x", 1) == 1); }
static void drain(int fd) { char c; assert(read(f_get(fd)->real_fd, &c, 1) == 1 && c == 'x'); }
static void edges(bool socket, bool initially_ready) {
    int fds[2]; pair(socket, fds);
    int ep = sys_epoll_create(0); assert(ep >= 0);
    if (initially_ready) fill(fds[1]);
    ctl(ep, EPOLL_CTL_ADD, fds[0], POLL_READ | POLL_EDGETRIGGERED, 1);
    if (!initially_ready) { wait_events(ep, 32, 0); fill(fds[1]); }
    assert(wait_events(ep, 32, 1) == 2);
    // A queued first event must not produce a duplicate while still ready.
    for (int i = 0; i < 3; i++) wait_events(ep, 32, 0);
    // Crucially, NO wait occurs between drain and refill. Only the kernel
    // sees that transition; a userspace readiness snapshot cannot infer it.
    for (int i = 0; i < 8; i++) {
        drain(fds[0]); fill(fds[1]);
        assert(wait_events(ep, 32, 1) == 2);
        wait_events(ep, 32, 0);
    }
    drain(fds[0]); wait_events(ep, 32, 0);
    assert(!f_close(ep) && !f_close(fds[0]) && !f_close(fds[1]));
}
static void duplicates(bool socket, bool oneshot) {
    int fds[2]; pair(socket, fds);
    int dup = sys_dup(fds[0]), ep = sys_epoll_create(0); assert(dup >= 0 && ep >= 0);
    uint32_t types = POLL_READ | POLL_EDGETRIGGERED | (oneshot ? POLL_ONESHOT : 0);
    ctl(ep, EPOLL_CTL_ADD, fds[0], types, 1);
    ctl(ep, EPOLL_CTL_ADD, dup, types, 2);
    fill(fds[1]);
    assert(wait_events(ep, 32, 2) == 6);
    wait_events(ep, 32, 0);
    drain(fds[0]); fill(fds[1]);
    assert(wait_events(ep, 32, oneshot ? 0 : 2) == (oneshot ? 0 : 6));
    if (oneshot) {
        ctl(ep, EPOLL_CTL_MOD, fds[0], types, 3);
        assert(wait_events(ep, 32, 1) == 8);
        wait_events(ep, 32, 0);
        ctl(ep, EPOLL_CTL_MOD, dup, types, 4);
        assert(wait_events(ep, 32, 1) == 16);
        wait_events(ep, 32, 0);
    } else {
        wait_events(ep, 32, 0);
        drain(fds[0]); fill(fds[1]);
        // Result capacity must not consume the second dup's pending edge.
        uint64_t a = wait_events(ep, 1, 1), b = wait_events(ep, 1, 1);
        assert((a | b) == 6 && !(a & b));
        wait_events(ep, 32, 0);
    }
    drain(fds[0]);
    assert(!f_close(ep) && !f_close(dup) && !f_close(fds[0]) && !f_close(fds[1]));
}
static void mixed(bool socket) {
    int fds[2]; pair(socket, fds);
    int dup = sys_dup(fds[0]), ep = sys_epoll_create(0); assert(dup >= 0 && ep >= 0);
    uint32_t et = POLL_READ | POLL_EDGETRIGGERED;
    ctl(ep, EPOLL_CTL_ADD, fds[0], et | POLL_ONESHOT, 1);
    ctl(ep, EPOLL_CTL_ADD, dup, et, 2);
    fill(fds[1]); assert(wait_events(ep, 32, 2) == 6);
    wait_events(ep, 32, 0); // disabling one dup must not queue the other's edge
    ctl(ep, EPOLL_CTL_MOD, fds[0], et | POLL_ONESHOT, 3);
    assert(wait_events(ep, 32, 1) == 8); // re-arm ONLY this ready registration
    wait_events(ep, 32, 0);
    drain(fds[0]); fill(fds[1]);
    assert(wait_events(ep, 32, 1) == 4);
    wait_events(ep, 32, 0);
    drain(fds[0]);
    assert(!f_close(ep) && !f_close(dup) && !f_close(fds[0]) && !f_close(fds[1]));
}
struct wait_job { struct task *task; int ep, result; };
static void *blocked_waiter(void *arg) {
    struct wait_job *j = arg;
    current = j->task; blocking_test = true; backend_calls = 0;
    j->result = sys_epoll_wait(j->ep, output, 32, 1000);
    assert(!current->blocking && !current->sockrestart.count && list_empty(&current->sockrestart.listen));
    current = NULL;
    return NULL;
}
static void sleeping_mod(bool socket, bool oneshot) {
    int fds[2]; pair(socket, fds);
    int ep = sys_epoll_create(0), dup = -1; assert(ep >= 0);
    uint32_t et = POLL_READ | POLL_EDGETRIGGERED;
    uint32_t types = et | (oneshot ? POLL_ONESHOT : 0);
    ctl(ep, EPOLL_CTL_ADD, fds[0], types, 1);
    if (oneshot) {
        dup = sys_dup(fds[0]); assert(dup >= 0);
        // Keep the host union unchanged while the oneshot is disabled/rearmed.
        ctl(ep, EPOLL_CTL_ADD, dup, et, 2);
    }
    fill(fds[1]);
    assert(wait_events(ep, 32, oneshot ? 2 : 1) == (oneshot ? 6 : 2));
    wait_events(ep, 32, 0); // consume all real edges, leave the fd ready
    assert(!pthread_barrier_init(&entered, NULL, 2));
    assert(!pthread_barrier_init(&resume, NULL, 2));
    struct wait_job j = {.task = current, .ep = ep};
    pthread_t t; assert(!pthread_create(&t, NULL, blocked_waiter, &j));
    barrier(&entered);
    struct poll *p = f_get(ep)->epollfd.poll;
    lock(&p->lock);
    assert(p->waiters == 1 && p->notify_pipe[1] >= 0 && current->blocking);
    unlock(&p->lock);
    ctl(ep, EPOLL_CTL_MOD, fds[0], types, 3);
    barrier(&resume); assert(!pthread_join(t, NULL));
    assert(j.result == 1);
    struct guest_event ev; assert(!user_get(output, ev));
    assert(ev.events == POLL_READ && ev.data == 3);
    assert(!pthread_barrier_destroy(&entered) && !pthread_barrier_destroy(&resume));
    // Neither the MOD nor its notify wake may rearm the still-ready dup.
    wait_events(ep, 32, 0);
    drain(fds[0]); fill(fds[1]);
    assert(wait_events(ep, 32, 1) == (oneshot ? 4 : 8));
    wait_events(ep, 32, 0);
    drain(fds[0]);
    if (dup >= 0) assert(!f_close(dup));
    assert(!f_close(ep) && !f_close(fds[0]) && !f_close(fds[1]));
}
static void rejected_ready(void) {
    int fds[2]; pair(false, fds);
    int ep = sys_epoll_create(0); assert(ep >= 0);
    ctl(ep, EPOLL_CTL_ADD, fds[0], POLL_PRI | POLL_EDGETRIGGERED, 1);
    struct poll *p = f_get(ep)->epollfd.poll;
    struct poll_fd *pf = list_first_entry(&p->poll_fds, struct poll_fd, fds);
    // Model a backend readiness/guest-interest mismatch with real level-ready
    // kernel events, not injected epoll_wait results. Harvest must be bounded.
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = pf};
    assert(!epoll_ctl(p->real.fd, EPOLL_CTL_MOD, f_get(fds[0])->real_fd, &ev));
    fill(fds[1]);
    for (int i = 0; i < 8; i++) {
        wait_events(ep, 32, 0);
        assert(backend_calls == 1);
    }
    drain(fds[0]);
    assert(!f_close(ep) && !f_close(fds[0]) && !f_close(fds[1]));
}
static void batches(void) {
    int ep = sys_epoll_create(0), fds[12][2]; assert(ep >= 0);
    for (int i = 0; i < 12; i++) {
        pair(i & 1, fds[i]);
        ctl(ep, EPOLL_CTL_ADD, fds[i][0], POLL_READ | POLL_EDGETRIGGERED, i);
        fill(fds[i][1]);
    }
    assert(wait_events(ep, 32, 12) == 4095);
    wait_events(ep, 32, 0);
    for (int i = 0; i < 12; i++) { drain(fds[i][0]); fill(fds[i][1]); }
    assert(wait_events(ep, 32, 12) == 4095);
    wait_events(ep, 32, 0);
    assert(!f_close(ep));
    for (int i = 0; i < 12; i++) assert(!f_close(fds[i][0]) && !f_close(fds[i][1]));
}
int main(void) {
    alarm(15);
    assert(!mount_root(&realfs, "/") && !become_first_process());
    current->group->limits[RLIMIT_NOFILE_].cur = 256;
    write_wrlock(&current->mem->lock);
    assert(!pt_map_nothing(current->mem, input >> PAGE_BITS, 1, P_READ | P_WRITE));
    write_wrunlock(&current->mem->lock);
    for (int socket = 0; socket < 2; socket++) {
        edges(socket, false); edges(socket, true);
        duplicates(socket, false); duplicates(socket, true); mixed(socket);
        sleeping_mod(socket, false); sleeping_mod(socket, true);
    }
    batches(); rejected_ready();
    puts("poll-edge-ok: real kernel zero-timeout pipe/socket ET drain/refill, no stale first-edge duplicate, dup fanout/result capacity, mixed ET oneshot/MOD rearm, barrier-blocked same-union MOD notify (ET and mixed oneshot dup), multi-batch harvest, bounded rejected level-ready wakes");
}

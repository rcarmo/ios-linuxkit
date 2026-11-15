// Exercise the real poll lookup while proving that it acquires poll->lock.
// This is an internal-list test, not a claim that fd lifetime races are fixed.
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "fs/poll.h"
static struct poll *poller;
static struct fd member;
static _Atomic bool started, entered, finished;
static _Atomic bool tracked;
static pthread_t worker;
static _Thread_local bool is_worker;
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (atomic_load(&tracked) && is_worker && mutex == &poller->lock.m)
        atomic_store(&entered, true);
    return __real_pthread_mutex_lock(mutex);
}
static void *lookup(void *unused) {
    (void)unused;
    is_worker = true;
    atomic_store(&started, true);
    assert(poll_has_fd(poller, &member, 7));
    atomic_store(&finished, true);
    return NULL;
}
int main(void) {
    alarm(10);
    poller = poll_create(); assert(!IS_ERR(poller));
    // Avoid host fd registration: only guest-list membership is under test.
    static const struct fd_ops ops = {0};
    member.ops = &ops; lock_init(&member.poll_lock); list_init(&member.poll_fds);
    assert(poll_add_fd(poller, &member, 7, POLL_READ, (union poll_fd_info){.num=42}) == 0);
    assert(!poll_has_fd(poller, &member, 8));
    lock(&poller->lock);
    atomic_store(&tracked, true);
    assert(!pthread_create(&worker, NULL, lookup, NULL));
    while (!atomic_load(&started)) sched_yield();
    // Wait until lookup either attempts this lock or wrongly completes without it.
    struct timespec begin, now; assert(!clock_gettime(CLOCK_MONOTONIC, &begin));
    while (!atomic_load(&entered) && !atomic_load(&finished)) {
        sched_yield(); assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        assert(now.tv_sec - begin.tv_sec < 5);
    }
    assert(atomic_load(&entered) && !atomic_load(&finished));
    unlock(&poller->lock);
    assert(!pthread_join(worker, NULL) && atomic_load(&finished));
    atomic_store(&tracked, false);
    assert(!poll_del_fd(poller, &member, 7));
    assert(!poll_has_fd(poller, &member, 7));
    poll_destroy(poller);
    puts("poll-membership-lock-ok");
}

// Actual task/signal/token lifecycle. No host kill or async thread cancellation.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "kernel/native_offload.h"
#include "kernel/signal.h"
#include "fs/real.h"

static struct task *target;
static pthread_barrier_t barrier;
static _Thread_local bool transition_sender;
static _Thread_local unsigned lock_attempts;
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (transition_sender && mutex == &target->sighand->lock.m && ++lock_attempts == 2) {
        // Pause after the early request missed, before ordinary queuing takes
        // its lock. Main publishes here; the locked queue check must see it.
        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);
    }
    return __real_pthread_mutex_lock(mutex);
}
static void *send_between(void *unused) {
    (void)unused; transition_sender = true;
    send_signal(target, SIGTERM_, SIGINFO_NIL);
    transition_sender = false;
    return NULL;
}
static void *send_term(void *unused) {
    (void)unused;
    pthread_barrier_wait(&barrier);
    send_signal(target, SIGTERM_, SIGINFO_NIL);
    return NULL;
}
static void queue_clear(struct task *task) {
    lock(&task->sighand->lock);
    struct sigqueue *q, *tmp;
    list_for_each_entry_safe(&task->queue, q, tmp, queue) {
        list_remove(&q->queue); free(q);
    }
    task->pending = 0;
    unlock(&task->sighand->lock);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    target = current;
    target->thread = pthread_self();
    // Exercise both same-thread delivery and a real concurrent sender.
    struct native_cancel token, other;
    assert(native_cancel_begin(current, &token));
    assert(!native_cancel_begin(current, &other));
    current->blocked = sig_mask(SIGTERM_);
    send_signal(current, SIGTERM_, SIGINFO_NIL);
    assert(native_cancel_signal(&token) == 0 && sigset_has(current->pending, SIGTERM_));
    queue_clear(current); current->blocked = 0;
    current->sighand->action[SIGINT_].handler = SIG_IGN_;
    send_signal(current, SIGINT_, SIGINFO_NIL);
    assert(native_cancel_signal(&token) == 0);
    queue_clear(current); current->sighand->action[SIGINT_].handler = SIG_DFL_;
    assert(!native_cancel_request(current, SIGUSR1_));
    send_signal(current, SIGTERM_, SIGINFO_NIL);
    assert(native_cancel_signal(&token) == SIGTERM_);
    send_signal(current, SIGINT_, SIGINFO_NIL);
    assert(native_cancel_signal(&token) == SIGTERM_);
    current->blocked = sig_mask(SIGKILL_);
    send_signal(current, SIGKILL_, SIGINFO_NIL);
    assert(native_cancel_finish(current, &token) == SIGKILL_);
    current->blocked = 0; queue_clear(current);
    // Pending before publication, unpublishing, and reuse/fork ownership.
    send_signal(current, SIGTERM_, SIGINFO_NIL);
    assert(native_cancel_begin(current, &token));
    assert(native_cancel_finish(current, &token) == SIGTERM_);
    queue_clear(current);
    assert(!native_cancel_request(current, SIGTERM_));
    assert(native_cancel_begin(current, &token));
    current->native_pid = 99; current->is_native_proxy = true;
    assert(become_new_init_child() == 0);
    struct task *child = current;
    assert(!child->native_cancel && !child->native_pid && !child->is_native_proxy);
    current = target;
    task_discard_unstarted(child);
    current->native_pid = 0; current->is_native_proxy = false;
    assert(native_cancel_finish(current, &token) == 0);
    unsigned before = 0, after = 0;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    pthread_t between; assert(pthread_create(&between, NULL, send_between, NULL) == 0);
    pthread_barrier_wait(&barrier);
    assert(native_cancel_begin(current, &token));
    pthread_barrier_wait(&barrier);
    assert(pthread_join(between, NULL) == 0);
    assert(native_cancel_finish(current, &token) == SIGTERM_);
    assert(!sigset_has(current->pending, SIGTERM_));
    for (unsigned i = 0; i < 1000; i++) {
        assert(native_cancel_begin(current, &token));
        pthread_t sender; assert(pthread_create(&sender, NULL, send_term, NULL) == 0);
        pthread_barrier_wait(&barrier);
        int sig = native_cancel_finish(current, &token);
        assert(pthread_join(sender, NULL) == 0);
        if (sig) { assert(sig == SIGTERM_); before++; }
        else { assert(sigset_has(current->pending, SIGTERM_)); after++; }
        queue_clear(current);
        assert(current->native_cancel == NULL);
    }
    pthread_barrier_destroy(&barrier);
    printf("native-cancel-lifecycle-ok races=1000 before=%u after=%u\n", before, after);
}

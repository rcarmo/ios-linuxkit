// Platform-independent lifecycle used by opt-in in-process native handlers.
#include "kernel/native_offload.h"
#include "kernel/task.h"
#include "kernel/signal.h"

static bool cancellable(struct task *task, int sig) {
    if (sig == SIGKILL_) return true;
    if (sig != SIGINT_ && sig != SIGTERM_ && sig != SIGHUP_ && sig != SIGQUIT_)
        return false;
    return !sigset_has(task->blocked, sig) &&
        task->sighand->action[sig].handler != SIG_IGN_;
}

static void request(struct native_cancel *cancel, int sig) {
    int old = atomic_load_explicit(&cancel->signal, memory_order_relaxed);
    if (!old || sig == SIGKILL_)
        atomic_store_explicit(&cancel->signal, sig, memory_order_release);
}

int native_cancel_signal(const struct native_cancel *cancel) {
    return atomic_load_explicit(&cancel->signal, memory_order_acquire);
}

bool native_cancel_begin(struct task *task, struct native_cancel *cancel) {
    if (!task->sighand) return false;
    lock(&task->sighand->lock);
    bool ok = task->native_cancel == NULL;
    if (ok) {
        atomic_init(&cancel->signal, 0);
        task->native_cancel = cancel;
        // Close the before-publication race. Pending signals remain in their
        // ordinary queue; only eligible cancellation is mirrored into the token.
        for (int sig = 1; sig < NUM_SIGS; sig++)
            if (sigset_has(task->pending, sig) && cancellable(task, sig))
                request(cancel, sig);
    }
    unlock(&task->sighand->lock);
    return ok;
}

bool native_cancel_request_locked(struct task *task, int sig) {
    bool accepted = task->native_cancel && cancellable(task, sig);
    if (accepted) request(task->native_cancel, sig);
    return accepted;
}

bool native_cancel_request(struct task *task, int sig) {
    if (!task->sighand) return false;
    lock(&task->sighand->lock);
    bool accepted = native_cancel_request_locked(task, sig);
    unlock(&task->sighand->lock);
    return accepted;
}

int native_cancel_finish(struct task *task, struct native_cancel *cancel) {
    lock(&task->sighand->lock);
    assert(task->native_cancel == cancel);
    // Signal requests and completion have a single linearization lock. After
    // withdrawal no signal path may retain or dereference this stack token.
    int sig = native_cancel_signal(cancel);
    task->native_cancel = NULL;
    unlock(&task->sighand->lock);
    return sig;
}

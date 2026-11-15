// Linux adapter: sampler I/O never runs while a guest memory lock is held.
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "kernel/memory_policy.h"
#include "kernel/memory_pressure_linux.h"

int memory_pressure_linux_feed(const struct memory_pressure_linux_options *options,
        struct memory_pressure_linux_snapshot *snapshot) {
    struct ish_memory_capture capture = ish_memory_policy_capture();
    struct memory_pressure_linux_snapshot value;
    if (memory_pressure_linux_sample(options, &value)) {
        ish_memory_policy_invalidate_capture(capture);
        return -1;
    }
    struct ish_memory_sample sample = {capture, value.allowance_bytes,
        value.available_bytes, value.critical};
    if (!ish_memory_policy_update(sample)) {
        ish_memory_policy_invalidate_capture(capture);
        errno = EAGAIN;
        return -1;
    }
    if (snapshot) *snapshot = value;
    return 0;
}

static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool started;
static atomic_bool stopping;
static void *sample_worker(void *unused) {
    (void)unused;
    struct memory_pressure_linux_options options;
    memory_pressure_linux_options_init(&options);
    options.safety_reserve_bytes = 64 * 1024 * 1024;
    while (!atomic_load(&stopping)) {
        struct timespec delay = {.tv_nsec = 250000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
        if (!atomic_load(&stopping)) memory_pressure_linux_feed(&options, NULL);
    }
    return NULL;
}
int memory_pressure_linux_start(void) {
    pthread_mutex_lock(&worker_lock);
    int error = 0;
    if (started) error = EALREADY;
    else if (!ish_memory_policy_enable(true)) error = EBUSY;
    else {
        struct memory_pressure_linux_options options;
        memory_pressure_linux_options_init(&options);
        options.safety_reserve_bytes = 64 * 1024 * 1024;
        if (memory_pressure_linux_feed(&options, NULL)) error = errno;
        else {
            atomic_store(&stopping, false);
            error = pthread_create(&worker, NULL, sample_worker, NULL);
            if (!error) started = true;
        }
        if (error) ish_memory_policy_enable(false);
    }
    pthread_mutex_unlock(&worker_lock);
    if (error) { errno = error; return -1; }
    return 0;
}
int memory_pressure_linux_stop(void) {
    pthread_mutex_lock(&worker_lock);
    if (started) {
        atomic_store(&stopping, true);
        pthread_join(worker, NULL);
        started = false;
    }
    bool disabled = ish_memory_policy_enable(false);
    pthread_mutex_unlock(&worker_lock);
    if (!disabled) { errno = EBUSY; return -1; }
    return 0;
}

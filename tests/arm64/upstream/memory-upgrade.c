// Controlled competing writers during real mem_ptr lock upgrade, no policy.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "fs/real.h"
static struct task *owner;
static int action, pending_action;
static bool contention;
static atomic_bool holder_ready, holder_done;
static pthread_t holder_thread;
static void *hold_read(void *unused) {
    (void)unused;
    read_wrlock(&owner->mem->lock);
    atomic_store(&holder_ready, true);
    // Force genuine contention, then allow the waiting upgrade to finish.
    struct timespec delay = {.tv_nsec = 20000000};
    nanosleep(&delay, NULL);
    read_wrunlock(&owner->mem->lock);
    atomic_store(&holder_done, true);
    return NULL;
}
static page_t fault = 0x100, anchor = 0x104;
static void *writer(void *unused) {
    (void)unused; current = owner;
    write_wrlock(&current->mem->lock);
    if (pending_action <= 3) {
        assert(pt_unmap_always(current->mem, fault, 4) == 0);
        if (pending_action == 2) assert(pt_map_lazy(current->mem, fault, 4, P_READ) == 0);
        if (pending_action == 3) assert(pt_map_lazy(current->mem, fault, 4, 0) == 0);
    } else if (pending_action == 4) {
        assert(pt_unmap(current->mem, anchor, 1) == 0);
    } else if (pending_action == 5) {
        assert(pt_set_flags(current->mem, anchor, 1, P_READ) == 0);
    } else if (pending_action == 6) {
        assert(pt_map_lazy(current->mem, fault, 4, 0) == 0);
    } else if (pending_action == 7) {
        assert(pt_map_nothing(current->mem, fault, 1, P_READ) == 0);
    } else if (pending_action == 8) {
        assert(pt_set_flags(current->mem, fault, 1, P_READ) == 0);
    } else if (pending_action == 9) {
        assert(pt_unmap(current->mem, fault, 1) == 0);
    } else if (pending_action == 10) {
        assert(pt_map_nothing(current->mem, fault, 1, P_READ | P_WRITE) == 0);
    } else {
        assert(pending_action == 11);
        assert(pt_set_flags(current->mem, fault, 1, 0) == 0);
    }
    write_wrunlock(&current->mem->lock); return NULL;
}
int __real_pthread_rwlock_trywrlock(pthread_rwlock_t *);
int __wrap_pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
    if (contention && current == owner && lock == &owner->mem->lock.l) {
        contention = false;
        assert(pthread_create(&holder_thread, NULL, hold_read, NULL) == 0);
        while (!atomic_load(&holder_ready)) sched_yield();
    }
    if (action && current == owner && lock == &owner->mem->lock.l) {
        pending_action = action;
        action = 0;
        pthread_t thread;
        int err = pthread_create(&thread, NULL, writer, NULL);
        if (err) fprintf(stderr, "pthread_create: %s\n", strerror(err));
        assert(err == 0);
        assert(pthread_join(thread, NULL) == 0);
    }
    return __real_pthread_rwlock_trywrlock(lock);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    owner = current;
    extern __thread volatile sig_atomic_t in_jit;
    bool jit = !strncmp(argv[1], "jit-", 4);
    const char *mode = jit ? argv[1] + 4 : argv[1];
    bool contended = !strcmp(mode, "contended");
    assert(!contended || jit);
    bool precedence = !strcmp(mode, "precedence");
    int scenario = contended ? 8 : precedence ? 0 : atoi(mode); assert(precedence || (scenario >= 1 && scenario <= 11));
    struct mem child; mem_init(&child);
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0, MEM_PAGES) == 0);
    assert(pt_map_nothing(current->mem, anchor, 1, P_READ | P_WRITE | P_GROWSDOWN) == 0);
    if (scenario <= 3 || precedence)
        assert(pt_map_lazy(current->mem, fault, 4, precedence ? P_READ : P_READ | P_WRITE) == 0);
    if (scenario >= 8) {
        assert(pt_map_nothing(current->mem, fault, 1, P_READ | P_WRITE) == 0);
        assert(pt_copy_on_write(current->mem, &child, fault, 1) == 0);
    }
    write_wrunlock(&current->mem->lock);
    // Synthetic address is intentionally distant from normal stack top.
    current->group->limits[RLIMIT_STACK_].cur = RLIM_INFINITY_;
    action = contended ? 0 : scenario;
    contention = contended;
    char value = 42;
    in_jit = jit;
    assert((user_write(fault << PAGE_BITS, &value, 1) == 0) == (scenario == 10 || contended));
    in_jit = 0;
    contention = false;
    struct pt_entry *pt = mem_pt(current->mem, fault);
    if (contended) {
        assert(pthread_join(holder_thread, NULL) == 0 && atomic_load(&holder_done));
        assert(pt && !(pt->flags & P_COW) && *(char *)pt->data->data == value);
    } else if (precedence || scenario == 2 || scenario == 7 || scenario == 8) {
        assert(pt && !(pt->flags & P_WRITE) && !(pt->flags & P_GROWSDOWN));
    } else if (scenario == 10) {
        assert(pt && !(pt->flags & P_COW) && *(char *)pt->data->data == value);
    } else if (scenario == 11) {
        assert(pt && !(pt->flags & P_RWX) && (pt->flags & P_COW));
    } else assert(!pt);
    if (precedence || scenario == 2 || scenario == 3 || scenario == 6) {
        struct mem_reservation *res = mem_find_reservation(current->mem, fault);
        assert(res && !(res->flags & P_WRITE));
    }
    // PROT_NONE permissions are host-wide on shared backing; do not read it.
    if (scenario >= 8 && scenario != 11) assert(*(char *)mem_pt(&child, fault)->data->data == 0);
    mem_destroy(&child);
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0, MEM_PAGES) == 0);
    write_wrunlock(&current->mem->lock);
    assert(atomic_load(&anon_page_count) == 0);
    puts("memory-upgrade-revalidation-ok");
}

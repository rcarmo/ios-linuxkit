// Actual archives, deterministic blocked host protection and mode transitions.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory_policy.h"
#include "fs/real.h"
#include "emu/tlb.h"
static pthread_barrier_t entered, resume;
static _Atomic int block_protect, fail_protect, fail_alloc;
static _Atomic int watch_waiter, waiter_seen;
int __real_sched_yield(void);
int __wrap_sched_yield(void) {
    if (atomic_load(&watch_waiter)) atomic_store(&waiter_seen, 1);
    return __real_sched_yield();
}
static int fail_unmap;
static void *orphan;
static size_t orphan_size;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (n == sizeof(struct mem_reservation) && atomic_load(&fail_alloc) &&
            atomic_fetch_sub(&fail_alloc, 1) == 1) return NULL;
    return __real_malloc(n);
}
int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *p, size_t n, int f) {
    if (atomic_exchange(&block_protect, 0)) {
        pthread_barrier_wait(&entered); pthread_barrier_wait(&resume);
    }
    if (atomic_load(&fail_protect) && atomic_fetch_sub(&fail_protect, 1) == 1) {
        errno = ENOMEM; return -1;
    }
    return __real_mprotect(p, n, f);
}
int __real_munmap(void *, size_t);
int __wrap_munmap(void *p, size_t n) {
    if (fail_unmap) {
        fail_unmap = 0; orphan = p; orphan_size = n; errno = ENOMEM; return -1;
    }
    return __real_munmap(p, n);
}
static void publish(uint64_t available) {
    struct ish_memory_sample s = {ish_memory_policy_capture(), available, available, false};
    assert(ish_memory_policy_update(s));
}
static uint64_t ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void *bench_worker(void *arg) {
    (void)arg;
    struct ish_memory_ticket t = {0};
    for (int i = 0; i < 100000; i++) {
        assert(anon_pages_reserve_ticket(1, &t)); anon_pages_finish_ticket(&t, false);
    }
    return NULL;
}
static void bench(void) {
    struct ish_memory_ticket t = {0};
    uint64_t start = ns();
    for (int i = 0; i < 500000; i++) {
        assert(anon_pages_reserve_ticket(1, &t)); anon_pages_finish_ticket(&t, false);
    }
    printf("disabled-ticket-ns %.2f\n", (double)(ns() - start) / 500000);
    pthread_t workers[4]; start = ns();
    for (int i = 0; i < 4; i++) assert(!pthread_create(&workers[i], NULL, bench_worker, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(workers[i], NULL));
    printf("disabled-ticket-4threads-ns %.2f\n", (double)(ns() - start) / 400000);
    struct mem *m = current->mem;
    assert(!pt_map_nothing(m, 0x100, 64, P_READ));
    uint64_t generation = m->mmu.changes;
    start = ns();
    for (int i = 0; i < 1000; i++) {
        assert(!pt_set_flags(m, 0x100, 64, 0));
        assert(!pt_set_flags(m, 0x100, 64, P_READ));
    }
    printf("mprotect-pair-ns %.2f generations %lu\n", (double)(ns() - start) / 1000,
        m->mmu.changes - generation);
    assert(!pt_unmap(m, 0x100, 64));
}
struct worker { struct mem *mem; int result; };
static void *upgrade(void *arg) {
    struct worker *w = arg;
    write_wrlock(&w->mem->lock);
    w->result = pt_set_flags(w->mem, 0x200, 1, P_READ);
    write_wrunlock(&w->mem->lock); return NULL;
}
static void *hold_ticket(void *arg) {
    (void)arg;
    struct ish_memory_ticket t = {0};
    assert(anon_pages_reserve_ticket(1, &t));
    assert(!anon_pages_reserve_ticket(1, &t));
    pthread_barrier_wait(&entered); pthread_barrier_wait(&resume);
    assert(!anon_pages_finish_ticket(&t, false)); return NULL;
}
static pthread_barrier_t race_start, race_done, race_clean;
static bool toggle_result, reserve_result;
static void *race_toggle(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_barrier_wait(&race_start);
        toggle_result = ish_memory_policy_enable(true);
        pthread_barrier_wait(&race_done); pthread_barrier_wait(&race_clean);
    }
    return NULL;
}
static void *race_reserve(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        struct ish_memory_ticket t = {0};
        pthread_barrier_wait(&race_start);
        reserve_result = anon_pages_reserve_ticket(1, &t);
        pthread_barrier_wait(&race_done); pthread_barrier_wait(&race_clean);
        if (reserve_result) anon_pages_finish_ticket(&t, false);
    }
    return NULL;
}
static void tests(void) {
    pthread_t a, b;
    assert(!pthread_barrier_init(&race_start, NULL, 3));
    assert(!pthread_barrier_init(&race_done, NULL, 3));
    assert(!pthread_barrier_init(&race_clean, NULL, 3));
    assert(!pthread_create(&a, NULL, race_toggle, NULL));
    assert(!pthread_create(&b, NULL, race_reserve, NULL));
    for (int i = 0; i < 1000; i++) {
        // Wait for the previous ticket's cleanup, then invalidate all samples.
        while (!ish_memory_policy_enable(false)) sched_yield();
        pthread_barrier_wait(&race_start); pthread_barrier_wait(&race_done);
        assert(toggle_result != reserve_result);
        pthread_barrier_wait(&race_clean);
    }
    assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL));
    assert(ish_memory_policy_enable(false));
    pthread_barrier_destroy(&race_start); pthread_barrier_destroy(&race_done);
    pthread_barrier_destroy(&race_clean);
    assert(!pthread_barrier_init(&entered, NULL, 2));
    assert(!pthread_barrier_init(&resume, NULL, 2));
    // A disabled transaction must prevent enable until its cleanup completes.
    assert(!pthread_create(&a, NULL, hold_ticket, NULL));
    pthread_barrier_wait(&entered);
    assert(ish_memory_policy_budget().transactions == 1);
    assert(!ish_memory_policy_enable(true));
    assert(!ish_memory_policy_enable(false));
    pthread_barrier_wait(&resume); assert(!pthread_join(a, NULL));
    assert(!atomic_load(&anon_page_count));
    assert(ish_memory_policy_enable(true));
    struct ish_memory_ticket t = {0};
    assert(!anon_pages_reserve_ticket(1, &t));
    publish(16 * real_page_size);
    struct mem *m = current->mem, child;
    assert(!pt_map_nothing(m, 0x200, 2, 0));
    mem_init(&child); assert(!pt_copy_on_write(m, &child, 0x200, 2));
    // First claim holds the full backing maximum through failed mprotect.
    struct worker wa = {m, 0}, wb = {&child, 0};
    atomic_store(&block_protect, 1); atomic_store(&fail_protect, 1);
    assert(!pthread_create(&a, NULL, upgrade, &wa));
    pthread_barrier_wait(&entered);
    assert(ish_memory_policy_budget().pending_bytes == 2 * real_page_size);
    assert(!ish_memory_policy_enable(false));
    atomic_store(&watch_waiter, 1);
    assert(!pthread_create(&b, NULL, upgrade, &wb));
    while (!atomic_load(&waiter_seen)) __real_sched_yield();
    atomic_store(&watch_waiter, 0);
    assert(ish_memory_policy_budget().pending_bytes == 2 * real_page_size);
    assert(!ish_memory_policy_budget().retained_bytes);
    pthread_barrier_wait(&resume);
    assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL));
    assert(wa.result == _ENOMEM && wb.result == 0);
    assert(ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
    assert(!ish_memory_policy_budget().pending_bytes);
    assert(!pt_set_flags(m, 0x200, 1, P_READ));
    for (int i = 0; i < 16; i++) {
        assert(!pt_set_flags(m, 0x200, 2, 0));
        assert(!pt_set_flags(m, 0x200, 2, P_READ));
    }
    assert(ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
    publish(16 * real_page_size);
    assert(ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
    assert(!pt_unmap(m, 0x200, 1));
    assert(ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
    mem_destroy(&child);
    fail_unmap = 1; assert(!pt_unmap(m, 0x201, 1));
    assert(orphan && ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
    assert(ish_memory_policy_enable(false)); assert(ish_memory_policy_enable(true));
    publish(16 * real_page_size);
    assert(ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
    assert(!__real_munmap(orphan, orphan_size));
    ish_memory_policy_release(2 * real_page_size);
    // Full maximum refusal rolls the CAS claim back; recovery is possible.
    assert(!pt_map_nothing(m, 0x200, 2, 0)); publish(real_page_size);
    assert(pt_set_flags(m, 0x200, 1, P_READ) == _ENOMEM);
    assert(!atomic_load(&mem_pt(m, 0x200)->data->policy_bytes));
    assert(!ish_memory_policy_budget().pending_bytes);
    publish(4 * real_page_size); assert(!pt_set_flags(m, 0x200, 1, P_READ));
    assert(!pt_unmap(m, 0x200, 2));
    assert(ish_memory_policy_enable(false));
    // Partial mprotect prefix and reservation split failure each publish once.
    assert(!pt_map_nothing(m, 0x300, 3, 0));
    struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
    tlb_attach(tlb, &m->mmu);
    uint64_t gen = m->mmu.changes; atomic_store(&fail_protect, 2);
    assert(pt_set_flags(m, 0x300, 3, P_READ) == _ENOMEM);
    assert(m->mmu.changes == gen + 1);
    assert(__atomic_load_n(&tlb->stale, __ATOMIC_ACQUIRE) == TLB_STALE);
    tlb_free(tlb);
    assert(mem_pt(m, 0x300)->flags & P_READ);
    assert(!(mem_pt(m, 0x301)->flags & P_READ));
    assert(!pt_unmap(m, 0x300, 3));
    assert(!pt_map_lazy(m, 0x300, 4, 0)); assert(!pt_map_lazy(m, 0x304, 4, 0));
    gen = m->mmu.changes; atomic_store(&fail_alloc, 2);
    assert(mem_set_reservation_flags(m, 0x302, 4, P_READ) == _ENOMEM);
    assert(m->mmu.changes == gen + 1);
    assert(mem_find_reservation(m, 0x304)->flags & P_READ);
    assert(!(mem_find_reservation(m, 0x302)->flags & P_READ));
    atomic_store(&fail_alloc, 0); assert(!pt_unmap_always(m, 0x300, 8));
    assert(!atomic_load(&anon_page_count));
    assert(!ish_memory_policy_budget().retained_bytes);
    pthread_barrier_destroy(&entered); pthread_barrier_destroy(&resume);
    puts("memory-policy-optim-ok");
}
int main(int argc, char **argv) {
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    if (argc == 2 && !strcmp(argv[1], "bench")) bench(); else tests();
}

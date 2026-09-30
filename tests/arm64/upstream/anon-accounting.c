// Actual page-table/syscall accounting, not the upstream standalone model.
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/memory_policy.h"
#include "util/timer.h"
#include "fs/real.h"
static bool fail_data, fail_map, fail_protect;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (fail_data && n == sizeof(struct data)) { fail_data = false; return NULL; }
    return __real_malloc(n);
}
void *__real_mmap64(void *, size_t, int, int, int, off_t);
void *__wrap_mmap64(void *a, size_t n, int p, int f, int fd, off_t o) {
    if (fail_map) { fail_map = false; errno = ENOMEM; return MAP_FAILED; }
    return __real_mmap64(a, n, p, f, fd, o);
}
int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *a, size_t n, int p) {
    if (fail_protect) { fail_protect = false; errno = ENOMEM; return -1; }
    return __real_mprotect(a, n, p);
}
static uint64_t sampler_time;
static void *sample_writer(void *unused) {
    (void) unused;
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t allowance = (i & 1) ? 100 * PAGE_SIZE : 200 * PAGE_SIZE;
        assert(ish_memory_policy_update((struct ish_memory_sample) {
            allowance, allowance / 2, sampler_time, false, ish_memory_policy_capture()}));
    }
    return NULL;
}
static void *sample_reader(void *unused) {
    (void) unused;
    for (unsigned i = 0; i < 20000; i++) {
        struct ish_memory_sample sample; bool brake;
        assert(ish_memory_policy_snapshot(&sample, &brake));
        assert(sample.available_bytes == sample.allowance_bytes / 2 && !brake);
        assert(sample.monotonic_ms == sampler_time && !sample.critical);
    }
    return NULL;
}
#define ADMISSION_THREADS 12
static pthread_barrier_t admission_barrier;
static _Atomic unsigned admission_ok;
static void *admission_worker(void *unused) {
    (void) unused;
    struct ish_memory_ticket ticket = {0};
    bool ok = anon_pages_reserve_ticket(1, &ticket);
    if (ok) atomic_fetch_add(&admission_ok, 1);
    pthread_barrier_wait(&admission_barrier);
    pthread_barrier_wait(&admission_barrier);
    if (ok) anon_pages_finish_ticket(&ticket, false);
    return NULL;
}
static void feed_pressure(uint64_t available, bool critical) {
    struct ish_memory_capture capture = ish_memory_policy_capture();
    struct timespec clock = timespec_now(CLOCK_MONOTONIC);
    assert(ish_memory_policy_update((struct ish_memory_sample) {
        100 * real_page_size, available,
        (uint64_t) clock.tv_sec * 1000 + clock.tv_nsec / 1000000,
        critical, capture}));
}
static void feed(uint64_t available) {
    feed_pressure(available, false);
}
static void budget(uint64_t pending, uint64_t unsettled) {
    struct ish_memory_budget actual = ish_memory_policy_budget();
    assert(actual.pending_bytes == pending && actual.unsettled_bytes == unsettled);
}
static void count(long expected) {
    long actual = atomic_load(&anon_page_count);
    if (actual != expected) fprintf(stderr, "anon count: expected %ld got %ld\n", expected, actual);
    assert(actual == expected);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0);
    assert(become_first_process() == 0);
    count(0);
    fail_map = true;
    assert(pt_map_nothing(current->mem, 0x10, 3, P_READ) == _ENOMEM);
    assert(!fail_map); count(0);
    fail_data = true;
    assert(pt_map_nothing(current->mem, 0x10, 3, P_READ) == _ENOMEM);
    assert(!fail_data); count(0);
    // PROT_NONE is not charged, including fork and final unmap.
    assert(pt_map_nothing(current->mem, 0x10, 3, 0) == 0);
    count(0);
    assert(pt_unmap(current->mem, 0x10, 3) == 0);
    count(0);
    // Kernel-internal mappings must charge too, not just sys_mmap callers.
    assert(pt_map_nothing(current->mem, 0x10, 3, P_READ | P_WRITE) == 0);
    count(3);
    assert(pt_set_flags(current->mem, 0x10, 3, 0) == 0); count(0);
    fail_protect = true;
    assert(pt_set_flags(current->mem, 0x10, 3, P_READ | P_WRITE) == _ENOMEM);
    assert(!fail_protect); count(0);
    assert(!(mem_pt(current->mem, 0x10)->flags & P_RWX));
    assert(pt_set_flags(current->mem, 0x10, 3, P_READ | P_WRITE) == 0); count(3);
    assert(pt_unmap(current->mem, 0x11, 1) == 0); count(2);
    // Replacing an anonymous page does not count twice.
    assert(pt_map_nothing(current->mem, 0x10, 1, P_READ | P_WRITE) == 0); count(2);
    struct mem child;
    mem_init(&child);
    assert(pt_map_nothing(current->mem, 0x20, 1, 0) == 0);
    assert(pt_copy_on_write(current->mem, &child, 0x10, 0x20) == 0); count(4);
    // CoW replaces one charged page by another, not an additional mapping.
    char value = 42;
    assert(user_write(0x10000, &value, 1) == 0); count(4);
    mem_destroy(&child); count(2);
    assert(pt_unmap_always(current->mem, 0x10, 0x20) == 0); count(0);
    // A lazy reservation is charged on fault, then uncharged on removal.
    assert(pt_map_lazy(current->mem, 0x30, 16, P_READ | P_WRITE) == 0); count(0);
    assert(user_write(0x30000, &value, 1) == 0); count(1);
    assert(pt_unmap_always(current->mem, 0x30, 16) == 0); count(0);
    // High virtual addresses retain the local 48-bit reservation semantics.
    page_t high = ((addr_t)1 << 40) >> PAGE_BITS;
    assert(pt_map_lazy(current->mem, high, 16, P_READ | P_WRITE) == 0);
    assert(user_write(high << PAGE_BITS, &value, 1) == 0); count(1);
    assert(pt_unmap_always(current->mem, high, 16) == 0); count(0);
    // mmap/mremap callers must not double-charge the central map accounting.
    addr_t a = sys_mmap64(0x100000, 8192, P_READ | P_WRITE,
        MMAP_PRIVATE | MMAP_ANONYMOUS | MMAP_FIXED, -1, 0);
    assert(a == 0x100000); count(2);
    assert(sys_mremap(a, 8192, 12288, 0) == a); count(3);
    assert(sys_munmap(a, 12288) == 0); count(0);
    // Force mremap's move path by obstructing in-place growth.
    a = sys_mmap64(0x100000, 8192, P_READ | P_WRITE,
        MMAP_PRIVATE | MMAP_ANONYMOUS | MMAP_FIXED, -1, 0);
    assert(a == 0x100000);
    assert(pt_map_nothing(current->mem, PAGE(a) + 2, 1, P_READ) == 0); count(3);
    addr_t moved = sys_mremap(a, 8192, 16384, 1);
    assert(moved != a && (int64_t)moved > 0); count(5);
    assert(sys_munmap(moved, 16384) == 0); count(1);
    assert(pt_unmap(current->mem, PAGE(a) + 2, 1) == 0); count(0);
    // Failed cap reservation leaves no map or stale charge.
    atomic_store(&anon_page_count, ANON_MMAP_LIMIT_PAGES);
    assert(pt_map_nothing(current->mem, 0x40, 1, P_READ) == _ENOMEM);
    count(ANON_MMAP_LIMIT_PAGES);
    assert(mem_pt(current->mem, 0x40) == NULL);
    atomic_store(&anon_page_count, 0);
    // Experimental additional brake uses actual mapping admission, while the
    // hard guest ledger and rollback remain authoritative. No Apple feed here.
    ish_memory_policy_enable(true);
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == _ENOMEM); count(0);
    struct timespec clock = timespec_now(CLOCK_MONOTONIC);
    uint64_t now = (uint64_t) clock.tv_sec * 1000 + clock.tv_nsec / 1000000;
    const uint64_t allowance = 100 * PAGE_SIZE;
    struct ish_memory_sample sample = {allowance, 20 * PAGE_SIZE, now, false};
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == 0); count(1);
    assert(pt_unmap(current->mem, 0x50, 1) == 0); count(0);
    sample.available_bytes = 9 * PAGE_SIZE;
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == _ENOMEM); count(0);
    sample.available_bytes = 14 * PAGE_SIZE;
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1)); count(0); // Hysteresis retains brake.
    sample.available_bytes = 15 * PAGE_SIZE;
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(anon_pages_reserve(1)); anon_pages_unreserve(1); count(0);
    sample.critical = true;
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1));
    // PROT_NONE stays uncharged and allowed even when ordinary commits brake.
    assert(pt_map_nothing(current->mem, 0x50, 1, 0) == 0); count(0);
    assert(pt_unmap(current->mem, 0x50, 1) == 0);
    struct ish_memory_sample copy; bool braked;
    assert(ish_memory_policy_snapshot(&copy, &braked) && braked);
    sample.available_bytes = allowance + 1;
    assert(!ish_memory_policy_update(sample));
    sample.available_bytes = allowance; sample.monotonic_ms = now - 1;
    assert(!ish_memory_policy_update(sample));
    assert(ish_memory_policy_snapshot(&copy, &braked) && copy.critical);
    ish_memory_policy_enable(true);
    sample = (struct ish_memory_sample) {allowance, allowance, now - 3000, false};
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1)); count(0); // Stale fail closed.
    ish_memory_policy_enable(true);
    sample.monotonic_ms = now + 100000;
    sample.capture = ish_memory_policy_capture();
    assert(!ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1)); count(0); // Future sample rejected.
    ish_memory_policy_enable(true);
    clock = timespec_now(CLOCK_MONOTONIC);
    sample.monotonic_ms = (uint64_t) clock.tv_sec * 1000 + clock.tv_nsec / 1000000;
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(101)); count(0); // Larger than sampled availability.
    atomic_store(&anon_page_count, ANON_MMAP_LIMIT_PAGES);
    assert(!anon_pages_reserve(1)); // Pressure feed never bypasses the hard ceiling.
    atomic_store(&anon_page_count, 0);
    fail_data = true;
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == _ENOMEM); count(0);
    assert(mem_pt(current->mem, 0x50) == NULL);
    sample.available_bytes = allowance / 2;
    sample.capture = ish_memory_policy_capture();
    assert(ish_memory_policy_update(sample));
    sampler_time = sample.monotonic_ms;
    pthread_t writer, reader;
    assert(pthread_create(&writer, NULL, sample_writer, NULL) == 0);
    assert(pthread_create(&reader, NULL, sample_reader, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0 && pthread_join(reader, NULL) == 0);
    ish_memory_policy_enable(false);
    assert(anon_pages_reserve(1)); anon_pages_unreserve(1); count(0);
    // Explicit pending tickets serialize admission against one sample. Every
    // successful claim holds its slot until the barrier releases it for refund.
    assert(ish_memory_policy_enable(true));
    feed(100 * real_page_size);
    struct ish_memory_ticket held = {0};
    // Even a zero-cost transaction must prevent resetting the policy mid-flight.
    assert(anon_pages_reserve_ticket(0, &held));
    assert(!ish_memory_policy_enable(false)); budget(0, 0);
    anon_pages_finish_ticket(&held, false);
    assert(anon_pages_reserve_ticket(96 * (real_page_size / PAGE_SIZE), &held));
    budget(96 * real_page_size, 0);
    assert(!ish_memory_policy_enable(false)); // Cannot discard an in-flight claim.
    assert(pthread_barrier_init(&admission_barrier, NULL, ADMISSION_THREADS + 1) == 0);
    pthread_t workers[ADMISSION_THREADS];
    atomic_store(&admission_ok, 0);
    for (unsigned i = 0; i < ADMISSION_THREADS; i++)
        assert(pthread_create(&workers[i], NULL, admission_worker, NULL) == 0);
    pthread_barrier_wait(&admission_barrier);
    assert(atomic_load(&admission_ok) == 4);
    budget(100 * real_page_size, 0);
    feed(100 * real_page_size); // Publication cannot erase outstanding tickets.
    budget(100 * real_page_size, 0);
    struct ish_memory_ticket refused = {0};
    assert(!anon_pages_reserve_ticket(1, &refused));
    pthread_barrier_wait(&admission_barrier);
    for (unsigned i = 0; i < ADMISSION_THREADS; i++) assert(pthread_join(workers[i], NULL) == 0);
    assert(pthread_barrier_destroy(&admission_barrier) == 0);
    anon_pages_finish_ticket(&held, false); count(0); budget(0, 0);
    // A capture made while the ticket is outstanding cannot settle its later
    // completion, even when the sample is published after it commits.
    struct ish_memory_capture before = ish_memory_policy_capture();
    assert(anon_pages_reserve_ticket(1, &held));
    anon_pages_finish_ticket(&held, true); count(1); budget(0, real_page_size);
    clock = timespec_now(CLOCK_MONOTONIC);
    sample = (struct ish_memory_sample) {100 * real_page_size, 100 * real_page_size,
        (uint64_t) clock.tv_sec * 1000 + clock.tv_nsec / 1000000, false, before};
    assert(ish_memory_policy_update(sample)); budget(0, real_page_size);
    assert(!ish_memory_policy_update(sample)); // Captures are single-use.
    anon_pages_unreserve(1); count(0); budget(0, real_page_size);
    feed(100 * real_page_size); budget(0, 0);
    // Actual mapping failures refund debt as well as logical charge.
    fail_map = true;
    assert(pt_map_nothing(current->mem, 0x60, 1, P_READ) == _ENOMEM);
    assert(!fail_map); count(0); budget(0, 0);
    fail_data = true;
    assert(pt_map_nothing(current->mem, 0x60, 1, P_READ) == _ENOMEM);
    assert(!fail_data); count(0); budget(0, 0);
    assert(pt_map_nothing(current->mem, 0x60, 1, P_READ) == 0);
    count(1); budget(0, real_page_size);
    assert(pt_unmap(current->mem, 0x60, 1) == 0); count(0);
    budget(0, real_page_size); // Unmapping isn't proof that host RSS was reclaimed.
    feed(100 * real_page_size); budget(0, 0);
    // Critical pressure refuses lazy and growsdown commitments, not merely
    // explicit mmap. Recovery after a fresh high-headroom sample retries safely.
    assert(pt_map_lazy(current->mem, 0x70, 2, P_READ | P_WRITE) == 0);
    feed_pressure(100 * real_page_size, true);
    assert(user_write(0x70000, &value, 1) != 0);
    assert(mem_pt(current->mem, 0x70) == NULL); count(0);
    feed(100 * real_page_size);
    assert(user_write(0x70000, &value, 1) == 0); count(1);
    assert(pt_unmap_always(current->mem, 0x70, 2) == 0); count(0);
    assert(pt_map_nothing(current->mem, STACK_INIT_PAGE, 1,
        P_READ | P_WRITE | P_GROWSDOWN) == 0); count(1);
    feed_pressure(100 * real_page_size, true);
    addr_t below_stack = (STACK_INIT_PAGE - 1) << PAGE_BITS;
    assert(user_write(below_stack, &value, 1) != 0);
    assert(mem_pt(current->mem, STACK_INIT_PAGE - 1) == NULL); count(1);
    feed(100 * real_page_size);
    assert(user_write(below_stack, &value, 1) == 0); count(2);
    assert(pt_unmap_always(current->mem, STACK_INIT_PAGE - 1, 2) == 0); count(0);
    // Protection admission must precede changing flags or host protections.
    feed(100 * real_page_size); budget(0, 0);
    assert(pt_map_nothing(current->mem, 0x80, 1, 0) == 0); count(0);
    feed_pressure(100 * real_page_size, true);
    assert(pt_set_flags(current->mem, 0x80, 1, P_READ | P_WRITE) == _ENOMEM);
    assert(!(mem_pt(current->mem, 0x80)->flags & P_RWX)); count(0); budget(0, 0);
    feed(100 * real_page_size);
    fail_protect = true;
    assert(pt_set_flags(current->mem, 0x80, 1, P_READ | P_WRITE) == _ENOMEM);
    assert(!fail_protect); count(0); budget(0, 0);
    assert(!(mem_pt(current->mem, 0x80)->flags & P_RWX));
    assert(pt_set_flags(current->mem, 0x80, 1, P_READ | P_WRITE) == 0);
    count(1); budget(0, real_page_size);
    assert(pt_unmap(current->mem, 0x80, 1) == 0); count(0);
    feed(100 * real_page_size); budget(0, 0);
    // A range protection change may partially succeed. Publish the generation
    // even when the next page's admission fails, without leaking its ticket.
    assert(pt_map_nothing(current->mem, 0x80, 2, 0) == 0);
    assert(anon_pages_reserve_ticket(99 * (real_page_size / PAGE_SIZE), &held));
    unsigned generation = current->mem->mmu.changes;
    assert(pt_set_flags(current->mem, 0x80, 2, P_READ | P_WRITE) == _ENOMEM);
    assert(current->mem->mmu.changes != generation);
    assert(mem_pt(current->mem, 0x80)->flags & P_WRITE);
    assert(!(mem_pt(current->mem, 0x81)->flags & P_RWX));
    count(99 * (real_page_size / PAGE_SIZE) + 1);
    budget(99 * real_page_size, real_page_size);
    anon_pages_finish_ticket(&held, false); count(1);
    assert(pt_unmap(current->mem, 0x80, 2) == 0); count(0);
    feed(100 * real_page_size); budget(0, 0);
    // CoW consumes a host allocation ticket without a second logical charge.
    assert(pt_map_nothing(current->mem, 0x90, 1, P_READ | P_WRITE) == 0);
    struct mem cow_child; mem_init(&cow_child);
    assert(pt_copy_on_write(current->mem, &cow_child, 0x90, 1) == 0); count(2);
    feed(100 * real_page_size); budget(0, 0);
    fail_map = true;
    assert(user_write(0x90000, &value, 1) != 0);
    assert(!fail_map && (mem_pt(current->mem, 0x90)->flags & P_COW));
    count(2); budget(0, 0);
    fail_data = true;
    assert(user_write(0x90000, &value, 1) != 0);
    assert(!fail_data && (mem_pt(current->mem, 0x90)->flags & P_COW));
    count(2); budget(0, 0);
    feed_pressure(100 * real_page_size, true);
    assert(user_write(0x90000, &value, 1) != 0); count(2); budget(0, 0);
    feed(100 * real_page_size);
    assert(user_write(0x90000, &value, 1) == 0); count(2); budget(0, real_page_size);
    assert(*(char *)mem_pt(&cow_child, 0x90)->data->data == 0);
    mem_destroy(&cow_child); count(1);
    assert(pt_unmap(current->mem, 0x90, 1) == 0); count(0);
    // Ptrace writes to read-only anonymous backing may add P_WRITE only after
    // admission and allocation/install succeed. Refusal must preserve the map.
    feed(100 * real_page_size); budget(0, 0);
    assert(pt_map_nothing(current->mem, 0xa0, 1, P_READ) == 0); count(1);
    feed(100 * real_page_size);
    feed_pressure(100 * real_page_size, true);
    assert(user_write_task_ptrace(current, 0xa0000, &value, 1) != 0);
    assert(!(mem_pt(current->mem, 0xa0)->flags & P_WRITE)); count(1); budget(0, 0);
    feed(100 * real_page_size);
    fail_map = true;
    assert(user_write_task_ptrace(current, 0xa0000, &value, 1) != 0);
    assert(!fail_map && !(mem_pt(current->mem, 0xa0)->flags & P_WRITE));
    count(1); budget(0, 0);
    fail_data = true;
    assert(user_write_task_ptrace(current, 0xa0000, &value, 1) != 0);
    assert(!fail_data && !(mem_pt(current->mem, 0xa0)->flags & P_WRITE));
    count(1); budget(0, 0);
    assert(user_write_task_ptrace(current, 0xa0000, &value, 1) == 0);
    assert(mem_pt(current->mem, 0xa0)->flags & P_WRITE);
    assert(*(char *)mem_pt(current->mem, 0xa0)->data->data == value);
    count(1); budget(0, real_page_size);
    assert(pt_unmap(current->mem, 0xa0, 1) == 0); count(0);
    assert(ish_memory_policy_enable(false));
    puts("memory-policy-tickets-ok: concurrent bounds, capture cutoff, refunds, lazy/stack/CoW/ptrace recovery");
    puts("memory-policy-actual-kernel-ok: coherent feed, hysteresis, stale/future, hard cap, rollback");
    puts("anon-accounting-actual-kernel-ok");
    return 0;
}

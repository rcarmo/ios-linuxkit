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
            allowance, allowance / 2, sampler_time, false}));
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
    assert(ish_memory_policy_update(sample));
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == 0); count(1);
    assert(pt_unmap(current->mem, 0x50, 1) == 0); count(0);
    sample.available_bytes = 9 * PAGE_SIZE;
    assert(ish_memory_policy_update(sample));
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == _ENOMEM); count(0);
    sample.available_bytes = 14 * PAGE_SIZE;
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1)); count(0); // Hysteresis retains brake.
    sample.available_bytes = 15 * PAGE_SIZE;
    assert(ish_memory_policy_update(sample));
    assert(anon_pages_reserve(1)); anon_pages_unreserve(1); count(0);
    sample.critical = true;
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
    assert(ish_memory_policy_snapshot(&sample, &braked) && sample.critical);
    ish_memory_policy_enable(true);
    sample = (struct ish_memory_sample) {allowance, allowance, now - 3000, false};
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1)); count(0); // Stale fail closed.
    ish_memory_policy_enable(true);
    sample.monotonic_ms = now + 100000;
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(1)); count(0); // Future timestamp invalid at admission.
    ish_memory_policy_enable(true);
    clock = timespec_now(CLOCK_MONOTONIC);
    sample.monotonic_ms = (uint64_t) clock.tv_sec * 1000 + clock.tv_nsec / 1000000;
    assert(ish_memory_policy_update(sample));
    assert(!anon_pages_reserve(101)); count(0); // Larger than sampled availability.
    atomic_store(&anon_page_count, ANON_MMAP_LIMIT_PAGES);
    assert(!anon_pages_reserve(1)); // Pressure feed never bypasses the hard ceiling.
    atomic_store(&anon_page_count, 0);
    fail_data = true;
    assert(pt_map_nothing(current->mem, 0x50, 1, P_READ) == _ENOMEM); count(0);
    assert(mem_pt(current->mem, 0x50) == NULL);
    sample.available_bytes = allowance / 2;
    assert(ish_memory_policy_update(sample));
    sampler_time = sample.monotonic_ms;
    pthread_t writer, reader;
    assert(pthread_create(&writer, NULL, sample_writer, NULL) == 0);
    assert(pthread_create(&reader, NULL, sample_reader, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0 && pthread_join(reader, NULL) == 0);
    ish_memory_policy_enable(false);
    assert(anon_pages_reserve(1)); anon_pages_unreserve(1); count(0);
    puts("memory-policy-actual-kernel-ok: coherent feed, hysteresis, stale/future, hard cap, rollback");
    puts("anon-accounting-actual-kernel-ok");
    return 0;
}

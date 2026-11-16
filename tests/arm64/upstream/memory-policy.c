// Deterministic actual-core tests; fake monotonic time, not fake policy logic.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory_policy.h"
#include "fs/real.h"
static _Atomic uint64_t clock_ms = 100000;
static bool fail_data, fail_map, fail_protect, expect_cleanup, fail_unmap;
static void *orphan;
static size_t orphan_size;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t bytes) {
    if (fail_data && bytes == sizeof(struct data)) { fail_data = false; return NULL; }
    return __real_malloc(bytes);
}
void *__real_mmap64(void *, size_t, int, int, int, off_t);
void *__wrap_mmap64(void *p, size_t n, int prot, int flags, int fd, off_t offset) {
    if (fail_map) { fail_map = false; errno = ENOMEM; return MAP_FAILED; }
    return __real_mmap64(p, n, prot, flags, fd, offset);
}
int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *p, size_t n, int flags) {
    if (fail_protect) { fail_protect = false; errno = ENOMEM; return -1; }
    return __real_mprotect(p, n, flags);
}
int __real_munmap(void *, size_t);
int __wrap_munmap(void *p, size_t n) {
    if (expect_cleanup) {
        assert(ish_memory_policy_budget().pending_bytes >= PAGE_SIZE);
        expect_cleanup = false;
    }
    if (fail_unmap) {
        fail_unmap = false; orphan = p; orphan_size = n; errno = ENOMEM; return -1;
    }
    return __real_munmap(p, n);
}
static void reclaim_orphan(void) {
    assert(orphan && __real_munmap(orphan, orphan_size) == 0);
    ish_memory_policy_release(real_page_size);
    orphan = NULL;
}
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *t) {
    if (id != CLOCK_MONOTONIC) return __real_clock_gettime(id, t);
    uint64_t n = atomic_load(&clock_ms);
    *t = (struct timespec){n / 1000, (n % 1000) * 1000000};
    return 0;
}
static void publish(uint64_t allowance, uint64_t available, bool critical) {
    struct ish_memory_sample s = {ish_memory_policy_capture(), allowance, available, critical};
    assert(ish_memory_policy_update(s));
}
static bool brake(void) { bool b; assert(ish_memory_policy_snapshot(NULL, &b)); return b; }
static pthread_barrier_t ready, release_barrier;
static atomic_int winners;
static void *claim(void *unused) {
    (void)unused;
    struct ish_memory_ticket t = {0};
    bool ok = anon_pages_reserve_ticket(1, &t);
    if (ok) atomic_fetch_add(&winners, 1);
    pthread_barrier_wait(&ready);
    pthread_barrier_wait(&release_barrier);
    if (ok) anon_pages_finish_ticket(&t, false);
    return NULL;
}
static void *protect_shared(void *argument) {
    struct mem *mem = argument;
    write_wrlock(&mem->lock);
    assert(pt_set_flags(mem, 0x200, 1, P_READ) == 0);
    write_wrunlock(&mem->lock);
    return NULL;
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    struct ish_memory_ticket t = {0};
    assert(ish_memory_policy_enable(true));
    assert(!anon_pages_reserve_ticket(1, &t)); // no sample
    publish(10000, 1499, false); assert(brake());
    publish(10000, 1500, false); assert(!brake());
    publish(10000, 1000, false); assert(!brake());
    publish(10000, 999, false); assert(brake());
    publish(10000, 9000, true); assert(brake());
    publish(10000, 9000, false); assert(!brake());
    struct ish_memory_sample s = {ish_memory_policy_capture(), 10000, 9000, false};
    s.capture.monotonic_ms++; assert(!ish_memory_policy_update(s));
    s.capture.monotonic_ms--; assert(ish_memory_policy_update(s));
    assert(!ish_memory_policy_update(s)); // replay
    s.capture = ish_memory_policy_capture();
    s.available_bytes = 10001; assert(!ish_memory_policy_update(s));
    s.available_bytes = 9000;
    atomic_fetch_add(&clock_ms, 2001);
    assert(!ish_memory_policy_update(s));
    assert(!anon_pages_reserve_ticket(1, &t)); // stale accepted sample too
    publish(65536, 65536, false);
    struct ish_memory_capture old_capture = ish_memory_policy_capture();
    publish(65536, 65536, false);
    ish_memory_policy_invalidate_capture(old_capture);
    assert(anon_pages_reserve_ticket(1, &t));
    anon_pages_finish_ticket(&t, false); // obsolete failed feed did not erase new sample
    assert(!ish_memory_reserve_ticket(-1, 0, &t));
    assert(!ish_memory_reserve_ticket(0, UINT64_MAX, &t));
    assert(ish_memory_reserve_ticket(1, 1, &t));
    assert(t.bytes == real_page_size);
    assert(!ish_memory_reserve_ticket(0, 1, &t)); // cannot reuse active owner
    assert(!ish_memory_policy_enable(false));
    struct ish_memory_budget b = ish_memory_policy_budget();
    assert(b.pending_bytes == real_page_size && b.transactions == 1);
    uint64_t debt = anon_pages_finish_ticket(&t, true);
    assert(atomic_load(&anon_page_count) == 1);
    b = ish_memory_policy_budget(); assert(!b.pending_bytes && b.retained_bytes == debt);
    publish(65536, 65536, false);
    assert(ish_memory_policy_budget().retained_bytes == debt); // mmap is not residency proof
    assert(ish_memory_policy_enable(false));
    assert(ish_memory_policy_enable(true));
    assert(ish_memory_policy_budget().retained_bytes == debt); // mode reset cannot lose debt
    ish_memory_policy_release(debt); anon_pages_unreserve(1);
    publish(65536, 4 * real_page_size, false);
    pthread_t threads[16];
    assert(!pthread_barrier_init(&ready, NULL, 17));
    assert(!pthread_barrier_init(&release_barrier, NULL, 17));
    for (unsigned i = 0; i < 16; i++) assert(!pthread_create(&threads[i], NULL, claim, NULL));
    pthread_barrier_wait(&ready);
    assert(atomic_load(&winners) == 4);
    assert(ish_memory_policy_budget().pending_bytes == 4 * real_page_size);
    pthread_barrier_wait(&release_barrier);
    for (unsigned i = 0; i < 16; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&ready); pthread_barrier_destroy(&release_barrier);
    assert(!atomic_load(&anon_page_count));
    assert(!ish_memory_policy_budget().pending_bytes);
    ish_memory_policy_invalidate(); assert(!anon_pages_reserve_ticket(1, &t));
    publish(1 << 20, 1 << 20, false);
    fail_map = true;
    assert(pt_map_nothing(current->mem, 0x100, 1, P_READ) == _ENOMEM);
    assert(!fail_map && !ish_memory_policy_budget().pending_bytes && !atomic_load(&anon_page_count));
    fail_data = expect_cleanup = true;
    assert(pt_map_nothing(current->mem, 0x100, 1, P_READ) == _ENOMEM);
    assert(!fail_data && !expect_cleanup && !ish_memory_policy_budget().pending_bytes);
    assert(!atomic_load(&anon_page_count) && !ish_memory_policy_budget().retained_bytes);
    fail_data = expect_cleanup = fail_unmap = true;
    assert(pt_map_nothing(current->mem, 0x100, 1, P_READ) == _ENOMEM);
    assert(!fail_data && !expect_cleanup && !fail_unmap);
    assert(!atomic_load(&anon_page_count) && !ish_memory_policy_budget().pending_bytes);
    assert(ish_memory_policy_budget().retained_bytes == real_page_size);
    publish(1 << 20, 1 << 20, false);
    assert(ish_memory_policy_budget().retained_bytes == real_page_size);
    reclaim_orphan();
    assert(pt_map_nothing(current->mem, 0x100, 1, 0) == 0);
    fail_protect = true;
    assert(pt_set_flags(current->mem, 0x100, 1, P_READ) == _ENOMEM);
    assert(!fail_protect && !ish_memory_policy_budget().pending_bytes);
    assert(!atomic_load(&anon_page_count) && !ish_memory_policy_budget().retained_bytes);
    assert(pt_unmap(current->mem, 0x100, 1) == 0);
    assert(pt_map_nothing(current->mem, 0x100, 3, P_READ | P_WRITE) == 0);
    assert(ish_memory_policy_budget().retained_bytes == 3 * PAGE_SIZE);
    assert(pt_unmap(current->mem, 0x100, 1) == 0);
    assert(ish_memory_policy_budget().retained_bytes == 3 * PAGE_SIZE);
    assert(pt_unmap(current->mem, 0x101, 2) == 0);
    assert(!ish_memory_policy_budget().retained_bytes);
    assert(!atomic_load(&anon_page_count));
    // Failed cleanup of an uninstalled CoW/ptrace copy must not refund host debt.
    for (unsigned ptrace = 0; ptrace < 2; ptrace++) {
        struct mem other; mem_init(&other);
        assert(pt_map_nothing(current->mem, 0x180, 1, ptrace ? P_READ : P_READ | P_WRITE) == 0);
        if (!ptrace) assert(pt_copy_on_write(current->mem, &other, 0x180, 1) == 0);
        long count = atomic_load(&anon_page_count);
        struct data *original = mem_pt(current->mem, 0x180)->data;
        fail_data = expect_cleanup = fail_unmap = true;
        if (ptrace) {
            read_wrlock(&current->mem->lock);
            assert(!mem_ptr(current->mem, 0x180000, MEM_WRITE_PTRACE));
            read_wrunlock(&current->mem->lock);
        } else {
            char value = 42;
            assert(user_write(0x180000, &value, 1) != 0);
        }
        assert(!fail_data && !expect_cleanup && !fail_unmap);
        assert(mem_pt(current->mem, 0x180)->data == original);
        assert(atomic_load(&anon_page_count) == count);
        assert(!ish_memory_policy_budget().pending_bytes);
        assert(ish_memory_policy_budget().retained_bytes == 2 * real_page_size);
        reclaim_orphan();
        mem_destroy(&other);
        assert(pt_unmap(current->mem, 0x180, 1) == 0);
        assert(!ish_memory_policy_budget().retained_bytes);
        assert(!atomic_load(&anon_page_count));
    }
    // Independent address spaces charge logical aliases separately, but one
    // shared host backing acquires its maximum debt only once.
    assert(pt_map_nothing(current->mem, 0x200, 1, 0) == 0);
    struct mem children[8];
    for (unsigned i = 0; i < 8; i++) {
        mem_init(&children[i]);
        assert(pt_copy_on_write(current->mem, &children[i], 0x200, 1) == 0);
    }
    for (unsigned i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, protect_shared, &children[i]));
    for (unsigned i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&anon_page_count) == 8);
    assert(ish_memory_policy_budget().retained_bytes == real_page_size);
    for (unsigned i = 0; i < 8; i++) mem_destroy(&children[i]);
    assert(!atomic_load(&anon_page_count));
    assert(ish_memory_policy_budget().retained_bytes == real_page_size);
    assert(pt_unmap(current->mem, 0x200, 1) == 0);
    assert(!ish_memory_policy_budget().retained_bytes);
    assert(ish_memory_policy_enable(false));
    puts("memory-policy-actual-core-ok");
}

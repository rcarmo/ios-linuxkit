// Actual local ticket-backed pt_map_cluster/page tables. Host-span adapter only;
// not a duplicate clustering model, Darwin protection or footprint proof.
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/memory_policy.h"
#include "util/timer.h"
#include "fs/real.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
static bool tracking;
static unsigned fail_maps, fail_objects, maps, unmaps;
static bool require_pending_unmap;
static size_t live_bytes;
struct allocation { void *ptr; size_t bytes; };
static struct allocation allocations[1024];
void *__real_mmap64(void *, size_t, int, int, int, off_t);
int __real_munmap(void *, size_t);
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (tracking && fail_objects && n == sizeof(struct data)) {
        fail_objects--; require_pending_unmap = true; return NULL;
    }
    return __real_malloc(n);
}
void *__wrap_mmap64(void *a, size_t n, int prot, int flags, int fd, off_t o) {
    if (!tracking) return __real_mmap64(a, n, prot, flags, fd, o);
    assert(!a && n && (flags & MAP_ANONYMOUS) && !(flags & MAP_FIXED));
    if (fail_maps) { fail_maps--; errno = ENOMEM; return MAP_FAILED; }
    size_t span = real_page_size;
    size_t rounded = (n + span - 1) / span * span;
    char *raw = __real_mmap64(NULL, rounded + span, prot, flags, fd, o);
    if (raw == MAP_FAILED) return raw;
    char *aligned = (char *)(((uintptr_t)raw + span - 1) & ~(span - 1));
    size_t prefix = aligned - raw, suffix = span - prefix;
    if (prefix) assert(__real_munmap(raw, prefix) == 0);
    if (suffix) assert(__real_munmap(aligned + rounded, suffix) == 0);
    unsigned i; for (i = 0; i < 1024 && allocations[i].ptr; i++);
    assert(i < 1024);
    allocations[i] = (struct allocation){aligned, rounded};
    maps++; live_bytes += rounded;
    return aligned;
}
int __wrap_munmap(void *a, size_t n) {
    if (!tracking) return __real_munmap(a, n);
    for (unsigned i = 0; i < 1024; i++) if (allocations[i].ptr == a) {
        if (require_pending_unmap) {
            assert(ish_memory_policy_budget().pending_bytes >= real_page_size);
            require_pending_unmap = false;
        }
        assert(n && n <= allocations[i].bytes);
        size_t bytes = allocations[i].bytes;
        int rc = __real_munmap(a, bytes);
        assert(rc == 0); allocations[i].ptr = NULL;
        unmaps++; live_bytes -= bytes; return rc;
    }
    // Unrelated runtime mappings are not a cluster ownership claim.
    return __real_munmap(a, n);
}
static unsigned race_action, replacement_action;
static struct task *race_task;
int __real_pthread_rwlock_trywrlock(pthread_rwlock_t *);
static void *race_writer(void *unused) {
    (void)unused;
    current = race_task;
    // A real competing writer runs between the fault's read unlock and write
    // acquisition, including freeing the reservation the reader used to see.
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0x100, 4) == 0);
    if (replacement_action == 2)
        assert(pt_map_lazy(current->mem, 0x100, 4, P_READ) == 0);
    write_wrunlock(&current->mem->lock);
    return NULL;
}
int __wrap_pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
    if (race_action && lock == &current->mem->lock.l) {
        race_task = current;
        replacement_action = race_action;
        race_action = 0;
        pthread_t thread;
        assert(pthread_create(&thread, NULL, race_writer, NULL) == 0);
        assert(pthread_join(thread, NULL) == 0);
    }
    return __real_pthread_rwlock_trywrlock(lock);
}
static void feed(bool critical) {
    struct ish_memory_capture capture = ish_memory_policy_capture();
    struct timespec clock = timespec_now(CLOCK_MONOTONIC);
    assert(ish_memory_policy_update((struct ish_memory_sample) {
        100 * real_page_size, 100 * real_page_size,
        (uint64_t)clock.tv_sec * 1000 + clock.tv_nsec / 1000000, critical, capture}));
}
static void *lazy_worker(void *arg) {
    current = race_task;
    page_t page = (page_t)(uintptr_t)arg;
    char value = (char)page;
    assert(user_write(page << PAGE_BITS, &value, 1) == 0);
    return NULL;
}
static void count(long n) { assert(atomic_load(&anon_page_count) == n); }
static void clean(void) {
    count(0);
    assert(ish_memory_policy_budget().pending_bytes == 0 && !require_pending_unmap);
    if (live_bytes) fprintf(stderr, "leaked spans: maps=%u unmaps=%u live=%zu\n", maps, unmaps, live_bytes);
    assert(live_bytes == 0 && maps == unmaps);
}
static bool same_reservation(struct mem *mem, page_t page, void *ctx) {
    return ctx && mem_find_reservation(mem, page) == ctx;
}
static bool bounded_range(struct mem *mem, page_t p, void *ctx) {
    (void)mem; page_t page = *(page_t *)ctx;
    pages_t width = real_page_size / PAGE_SIZE;
    page_t base = page & ~(width - 1);
    return p >= base && p < base + width;
}
static pages_t cluster(page_t page, bool predicate) {
    pages_t committed = 999;
    void *ctx = predicate ? mem_find_reservation(current->mem, page) : NULL;
    write_wrlock(&current->mem->lock);
    int rc = pt_map_cluster(current->mem, page, P_READ | P_WRITE,
        predicate ? same_reservation : bounded_range, predicate ? ctx : &page, &committed);
    write_wrunlock(&current->mem->lock);
    assert(rc == 0);
    return committed;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    size_t span = strtoul(argv[1], NULL, 10);
    assert(span == 4096 || span == 16384);
    const char *mode = argv[2];
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    assert(!current->mem->cluster_commits);
    real_page_size = span; tracking = true; current->mem->cluster_commits = true;
    pages_t width = span / PAGE_SIZE;
    if (!strcmp(mode, "basic")) {
        assert(pt_map_lazy(current->mem, 0x100, width, P_READ | P_WRITE) == 0);
        assert(cluster(0x100, true) == width); count(width);
        struct data *data = mem_pt(current->mem, 0x100)->data;
        assert(data->size == span && data->refcount == width && maps == 1);
        for (page_t p = 0; p < width; p++) {
            struct pt_entry *e = mem_pt(current->mem, 0x100 + p);
            assert(e->data == data && e->offset == p * PAGE_SIZE);
            memset((char *)data->data + e->offset, (int)p + 1, PAGE_SIZE);
            assert(pt_unmap(current->mem, 0x100 + p, 1) == 0);
            count(width - p - 1);
            assert(unmaps == (p + 1 == width));
        }
    } else if (!strcmp(mode, "boundary")) {
        assert(pt_map_lazy(current->mem, 0x101, 1, P_READ | P_WRITE) == 0);
        assert(pt_map_lazy(current->mem, 0x102, 1, P_READ) == 0);
        assert(cluster(0x101, true) == 1); count(1);
        assert(mem_pt(current->mem, 0x100) == NULL && mem_pt(current->mem, 0x102) == NULL);
        assert(pt_unmap_always(current->mem, 0x100, 4) == 0);
    } else if (!strcmp(mode, "hole")) {
        assert(cluster(0x100, false) == width);
        struct data *neighbour = mem_pt(current->mem, 0x100 + width - 1)->data;
        if (width > 1) {
            assert(pt_unmap(current->mem, 0x101, 1) == 0); count(width - 1);
            assert(unmaps == 0);
            assert(cluster(0x101, false) == 1); count(width);
            assert(mem_pt(current->mem, 0x103)->data == neighbour);
            assert(mem_pt(current->mem, 0x101)->data != neighbour && maps == 2);
        }
        assert(pt_unmap_always(current->mem, 0x100, width) == 0);
    } else if (!strcmp(mode, "fork")) {
        assert(cluster(0x100, false) == width);
        struct mem child; mem_init(&child);
        assert(pt_copy_on_write(current->mem, &child, 0x100, width) == 0); count(2 * width);
        struct data *shared = mem_pt(&child, 0x100)->data;
        assert(shared->refcount == 2 * width);
        char value = 42;
        assert(user_write(0x100000, &value, 1) == 0); count(2 * width);
        assert(*(char *)shared->data == 0);
        assert(maps == 2);
        assert(pt_unmap_always(current->mem, 0x100, width) == 0); count(width);
        assert(live_bytes == span && unmaps == 1);
        mem_destroy(&child);
    } else if (!strcmp(mode, "protect")) {
        assert(cluster(0x100, false) == width);
        unsigned generation = current->mem->mmu.changes;
        assert(pt_set_flags(current->mem, 0x100, 1, 0) == 0); count(width - 1);
        assert(current->mem->mmu.changes != generation);
        char value = 42;
        assert(user_write(0x100000, &value, 1) != 0);
        assert(pt_set_flags(current->mem, 0x100, 1, P_READ | P_WRITE) == 0); count(width);
        assert(user_write(0x100000, &value, 1) == 0);
        assert(pt_unmap_always(current->mem, 0x100, width) == 0);
    } else if (!strcmp(mode, "gpf-lazy") || !strcmp(mode, "gpf-stack")) {
        bool stack = !strcmp(mode, "gpf-stack");
        page_t fault = 0x100;
        if (stack) {
            assert(pt_map_nothing(current->mem, STACK_INIT_PAGE, 1,
                P_READ | P_WRITE | P_GROWSDOWN) == 0);
            fault = (STACK_INIT_PAGE & ~(width - 1)) - width;
        } else assert(pt_map_lazy(current->mem, fault, width, P_READ | P_WRITE) == 0);
        current->cpu.segfault_addr = fault << PAGE_BITS;
        current->cpu.segfault_was_write = true;
        current->cpu.pc = 0x4240;
        current->cpu.regs[0] = 0x12345678;
        handle_interrupt(INT_GPF);
        assert(current->cpu.pc == 0x4240 && current->cpu.regs[0] == 0x12345678);
        assert(mem_pt(current->mem, fault) != NULL); count(width + stack);
        assert(pt_unmap_always(current->mem, fault,
            stack ? STACK_INIT_PAGE - fault + 1 : width) == 0);
    } else if (!strcmp(mode, "tlb")) {
        assert(pt_map_lazy(current->mem, 0x100, width, P_READ | P_WRITE) == 0);
        struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
        tlb_refresh(tlb, &current->mem->mmu);
        char old = 42, replacement = 99, result;
        read_wrlock(&current->mem->lock);
        assert(tlb_write(tlb, 0x100000, &old, 1));
        read_wrunlock(&current->mem->lock); count(width);
        struct data *original = mem_pt(current->mem, 0x100)->data;
        write_wrlock(&current->mem->lock);
        assert(pt_unmap(current->mem, 0x100, 1) == 0);
        assert(pt_map_nothing(current->mem, 0x100, 1, P_READ | P_WRITE) == 0);
        write_wrunlock(&current->mem->lock);
        assert(user_write(0x100000, &replacement, 1) == 0);
        read_wrlock(&current->mem->lock);
        assert(tlb_read(tlb, 0x100000, &result, 1) && result == replacement);
        read_wrunlock(&current->mem->lock);
        if (width > 1) assert(*(char *)original->data == old);
        write_wrlock(&current->mem->lock);
        assert(pt_set_flags(current->mem, 0x100, 1, P_READ) == 0);
        write_wrunlock(&current->mem->lock);
        read_wrlock(&current->mem->lock);
        assert(!tlb_write(tlb, 0x100000, &old, 1));
        read_wrunlock(&current->mem->lock);
        tlb_free(tlb);
        assert(pt_unmap_always(current->mem, 0x100, width) == 0);
    } else if (!strcmp(mode, "existing")) {
        assert(cluster(0x100, false) == width);
        struct data *original = mem_pt(current->mem, 0x100)->data;
        pages_t committed = 999;
        assert(pt_map_cluster(current->mem, 0x100, P_READ, NULL, NULL, &committed) == _EINVAL);
        assert(committed == 0 && mem_pt(current->mem, 0x100)->data == original);
        count(width); assert(pt_unmap_always(current->mem, 0x100, width) == 0);
    } else if (!strcmp(mode, "disabled") || !strcmp(mode, "no-predicate")) {
        current->mem->cluster_commits = strcmp(mode, "disabled") != 0;
        assert(pt_map_lazy(current->mem, 0x100, width, P_READ | P_WRITE) == 0);
        pages_t committed;
        if (!strcmp(mode, "disabled")) assert(cluster(0x100, true) == 1);
        else {
            assert(pt_map_cluster(current->mem, 0x100, P_READ | P_WRITE,
                NULL, NULL, &committed) == 0 && committed == 1);
        }
        count(1); assert(maps == 1);
        assert(pt_unmap_always(current->mem, 0x100, width) == 0);
    } else if (!strcmp(mode, "lazy") || !strcmp(mode, "lazy-fail") ||
            !strcmp(mode, "lazy-none") || !strcmp(mode, "race-remove") ||
            !strcmp(mode, "race-protect") || !strcmp(mode, "concurrent")) {
        bool none = !strcmp(mode, "lazy-none");
        assert(pt_map_lazy(current->mem, 0x100, width, none ? 0 : P_READ | P_WRITE) == 0);
        char value = 42;
        if (!strcmp(mode, "lazy-fail")) fail_objects = width > 1 ? 1 : 0;
        if (!strcmp(mode, "race-remove")) race_action = 1;
        if (!strcmp(mode, "race-protect")) race_action = 2;
        unsigned generation = current->mem->mmu.changes;
        bool refusal = none || race_action;
        if (!strcmp(mode, "concurrent")) {
            race_task = current;
            pthread_t threads[12];
            for (unsigned i = 0; i < 12; i++) assert(pthread_create(&threads[i], NULL,
                lazy_worker, (void *)(uintptr_t)(0x100 + i % width)) == 0);
            for (unsigned i = 0; i < 12; i++) assert(pthread_join(threads[i], NULL) == 0);
        } else assert((user_write(0x100000, &value, 1) != 0) == refusal);
        if (none || !strcmp(mode, "race-remove")) { count(0); assert(maps == 0); }
        else if (!strcmp(mode, "race-protect")) {
            count(width);
            assert(!(mem_pt(current->mem, 0x100)->flags & P_WRITE));
        } else {
            count(!strcmp(mode, "lazy-fail") && width > 1 ? 1 : width);
            assert(current->mem->mmu.changes != generation);
        }
        assert(!fail_objects && !race_action);
        assert(pt_unmap_always(current->mem, 0x100, 4) == 0);
    } else if (!strcmp(mode, "stack") || !strcmp(mode, "stack-mid")) {
        assert(pt_map_nothing(current->mem, STACK_INIT_PAGE, 1,
            P_READ | P_WRITE | P_GROWSDOWN) == 0);
        page_t fault = (STACK_INIT_PAGE & ~(width - 1)) - width;
        if (!strcmp(mode, "stack-mid") && width > 1) fault++;
        char value = 42;
        assert(user_write(fault << PAGE_BITS, &value, 1) == 0);
        pages_t committed = !strcmp(mode, "stack-mid") || width == 1 ? 1 : width;
        count(committed + 1);
        assert(mem_pt(current->mem, fault - 1) == NULL);
        assert(pt_unmap_always(current->mem, fault, STACK_INIT_PAGE - fault + 1) == 0);
    } else if (!strcmp(mode, "cap") || !strcmp(mode, "pressure")) {
        page_t fault = 0x100;
        pages_t committed = 999;
        bool pressure = !strcmp(mode, "pressure");
        if (pressure) { assert(ish_memory_policy_enable(true)); feed(true); }
        else atomic_store(&anon_page_count, ANON_MMAP_LIMIT_PAGES - 1);
        int rc = pt_map_cluster(current->mem, fault, P_READ | P_WRITE,
            bounded_range, &fault, &committed);
        if (pressure) {
            assert(rc == _ENOMEM && committed == 0 && maps == 0); count(0);
            feed(false);
            assert(pt_map_lazy(current->mem, fault, width, P_READ | P_WRITE) == 0);
            char value = 42;
            assert(user_write(fault << PAGE_BITS, &value, 1) == 0); count(width);
            assert(ish_memory_policy_budget().unsettled_bytes == span);
            assert(pt_unmap_always(current->mem, fault, width) == 0);
            assert(ish_memory_policy_enable(false));
        } else {
            assert(rc == 0 && committed == 1); count(ANON_MMAP_LIMIT_PAGES);
            assert(pt_unmap(current->mem, fault, 1) == 0);
            count(ANON_MMAP_LIMIT_PAGES - 1); atomic_store(&anon_page_count, 0);
        }
    } else {
        assert(width == 4);
        bool data = !strncmp(mode, "data", 4);
        bool twice = strstr(mode, "double") != NULL;
        assert(data || !strncmp(mode, "mmap", 4));
        if (data) fail_objects = twice ? 2 : 1; else fail_maps = twice ? 2 : 1;
        pages_t committed = 999;
        page_t fault = 0x100;
        int rc = pt_map_cluster(current->mem, fault, P_READ | P_WRITE, bounded_range, &fault, &committed);
        if (twice) { assert(rc == _ENOMEM && committed == 0); count(0); }
        else {
            assert(rc == 0 && committed == 1); count(1);
            assert(mem_pt(current->mem, 0x101) == NULL);
            assert(pt_unmap(current->mem, 0x100, 1) == 0);
        }
        assert(!fail_maps && !fail_objects);
    }
    clean();
    printf("local-real-cluster-ok span=%zu mode=%s maps=%u unmaps=%u\n", span, mode, maps, unmaps);
    return 0;
}

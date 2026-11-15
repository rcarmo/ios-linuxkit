// Exercise real pt_set_flags: cap admission, refunds and partial publication.
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "fs/real.h"
static int fail_at, calls, fail_reservation_at, allocations;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if (size == sizeof(struct mem_reservation) && ++allocations == fail_reservation_at)
        return NULL;
    return __real_malloc(size);
}
int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *p, size_t n, int flags) {
    if (++calls == fail_at) { errno = ENOMEM; return -1; }
    return __real_mprotect(p, n, flags);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    struct mem *mem = current->mem;
    if (!strcmp(argv[1], "reservation")) {
        assert(pt_map_lazy(mem, 0x100, 4, 0) == 0);
        assert(pt_map_lazy(mem, 0x104, 4, 0) == 0);
        allocations = 0; fail_reservation_at = 2;
        unsigned changes = mem->mmu.changes;
        assert(pt_set_flags(mem, 0x102, 4, P_READ) == _ENOMEM);
        assert(mem_find_reservation(mem, 0x104)->flags & P_READ);
        assert(!(mem_find_reservation(mem, 0x102)->flags & P_READ));
        assert(mem->mmu.changes != changes);
        fail_reservation_at = 0;
        assert(pt_unmap_always(mem, 0x100, 8) == 0);
        puts("protection-admission-ok");
        return 0;
    }
    assert(pt_map_nothing(mem, 0x100, 3, 0) == 0);
    assert(atomic_load(&anon_page_count) == 0);
    bool cap = !strcmp(argv[1], "cap");
    assert(cap || !strcmp(argv[1], "partial") || !strcmp(argv[1], "first"));
    long base = cap ? ANON_MMAP_LIMIT_PAGES - 1 : 0;
    atomic_store(&anon_page_count, base);
    fail_at = cap ? 0 : !strcmp(argv[1], "first") ? 1 : 2;
    unsigned changes = mem->mmu.changes;
    assert(pt_set_flags(mem, 0x100, 3, P_READ | P_WRITE) == _ENOMEM);
    int done = fail_at == 1 ? 0 : 1;
    assert(atomic_load(&anon_page_count) == base + done);
    assert(!!(mem_pt(mem, 0x100)->flags & P_WRITE) == !!done);
    assert(!(mem_pt(mem, 0x101)->flags & P_RWX));
    assert(!(mem_pt(mem, 0x102)->flags & P_RWX));
    assert((mem->mmu.changes != changes) == !!done);
    // Releasing the synthetic cap and retrying must recover normally.
    atomic_store(&anon_page_count, done);
    fail_at = 0;
    assert(pt_set_flags(mem, 0x100, 3, P_READ | P_WRITE) == 0);
    assert(atomic_load(&anon_page_count) == 3);
    assert(pt_unmap(mem, 0x100, 3) == 0);
    assert(atomic_load(&anon_page_count) == 0);
    puts("protection-admission-ok");
}

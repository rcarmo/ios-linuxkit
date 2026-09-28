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
    puts("anon-accounting-actual-kernel-ok");
    return 0;
}

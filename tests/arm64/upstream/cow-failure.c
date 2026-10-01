// Allocation rollback in actual CoW/ptrace paths, independent of policy APIs.
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
#include "fs/real.h"
static bool fail_map, fail_data, tracking;
static void *live_copy;
static unsigned maps, unmaps;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (fail_data && n == sizeof(struct data)) { fail_data = false; return NULL; }
    return __real_malloc(n);
}
void *__real_mmap64(void *, size_t, int, int, int, off_t);
void *__wrap_mmap64(void *a, size_t n, int p, int f, int fd, off_t o) {
    if (fail_map) { fail_map = false; errno = ENOMEM; return MAP_FAILED; }
    void *copy = __real_mmap64(a, n, p, f, fd, o);
    if (tracking && copy != MAP_FAILED) { assert(!live_copy); live_copy = copy; maps++; }
    return copy;
}
int __real_munmap(void *, size_t);
int __wrap_munmap(void *p, size_t n) {
    if (p == live_copy) { live_copy = NULL; unmaps++; }
    return __real_munmap(p, n);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    bool ptrace = !strncmp(argv[1], "ptrace-", 7);
    const char *mode = ptrace ? argv[1] + 7 : argv[1];
    bool none = !strcmp(mode, "none"), success = !strcmp(mode, "success");
    assert(!none || ptrace);
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    unsigned flags = none ? 0 : ptrace ? P_READ : P_READ | P_WRITE;
    assert(pt_map_nothing(current->mem, 0x90, 1, flags) == 0);
    struct mem child; mem_init(&child);
    if (!ptrace) assert(pt_copy_on_write(current->mem, &child, 0x90, 1) == 0);
    struct pt_entry *entry = mem_pt(current->mem, 0x90);
    struct data *original = entry->data;
    unsigned old_flags = entry->flags;
    long pages = atomic_load(&anon_page_count);
    uint64_t changes = current->mem->mmu.changes;
    if (!strcmp(mode, "mmap")) fail_map = true;
    else if (!strcmp(mode, "data")) fail_data = true;
    else assert(none || success);
    char value = 42;
    tracking = true;
    if (ptrace) {
        read_wrlock(&current->mem->lock);
        char *p = mem_ptr(current->mem, 0x90000, MEM_WRITE_PTRACE);
        if (success) { assert(p); *p = value; } else assert(!p);
        read_wrunlock(&current->mem->lock);
    } else {
        assert((user_write(0x90000, &value, 1) == 0) == success);
    }
    tracking = false;
    entry = mem_pt(current->mem, 0x90);
    assert(!fail_map && !fail_data);
    assert(atomic_load(&anon_page_count) == pages);
    if (!success) {
        assert(entry->data == original && entry->flags == old_flags);
        assert(current->mem->mmu.changes == changes && !live_copy && maps == unmaps);
        if (!none) assert(*(char *)original->data == 0);
    } else {
        assert(entry->data != original && !(entry->flags & P_COW) && (entry->flags & P_WRITE));
        assert(*(char *)entry->data->data == value && current->mem->mmu.changes > changes);
    }
    if (!ptrace) assert(*(char *)mem_pt(&child, 0x90)->data->data == 0);
    mem_destroy(&child);
    assert(pt_unmap(current->mem, 0x90, 1) == 0);
    assert(atomic_load(&anon_page_count) == 0 && !live_copy && maps == unmaps);
    puts("cow-allocation-failure-ok");
}

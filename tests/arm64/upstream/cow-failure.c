// Isolate frozen-baseline CoW failure without requiring the new policy API.
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
static bool fail_map, fail_data;
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
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    assert(pt_map_nothing(current->mem, 0x90, 1, P_READ | P_WRITE) == 0);
    struct mem child; mem_init(&child);
    assert(pt_copy_on_write(current->mem, &child, 0x90, 1) == 0);
    if (!strcmp(argv[1], "mmap")) fail_map = true;
    else { assert(!strcmp(argv[1], "data")); fail_data = true; }
    char value = 42;
    assert(user_write(0x90000, &value, 1) != 0);
    assert(!fail_map && !fail_data);
    assert(mem_pt(current->mem, 0x90)->flags & P_COW);
    assert(atomic_load(&anon_page_count) == 2);
    assert(*(char *)mem_pt(&child, 0x90)->data->data == 0);
    mem_destroy(&child);
    assert(pt_unmap(current->mem, 0x90, 1) == 0);
    assert(atomic_load(&anon_page_count) == 0);
    puts("cow-allocation-failure-ok");
}

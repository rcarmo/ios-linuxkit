// Real upstream pt_map_cluster/page tables. Linux host-span adapter only;
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
#include "fs/real.h"
static bool tracking;
static unsigned fail_maps, fail_objects, maps, unmaps;
static size_t live_bytes;
struct allocation { void *ptr; size_t bytes; };
static struct allocation allocations[1024];
void *__real_mmap64(void *, size_t, int, int, int, off_t);
int __real_munmap(void *, size_t);
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (tracking && fail_objects && n == sizeof(struct data)) {
        fail_objects--; return NULL;
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
        assert(n && n <= allocations[i].bytes);
        size_t bytes = allocations[i].bytes;
        int rc = __real_munmap(a, bytes);
        assert(rc == 0); allocations[i].ptr = NULL;
        unmaps++; live_bytes -= bytes; return rc;
    }
    // Unrelated runtime mappings are not a cluster ownership claim.
    return __real_munmap(a, n);
}
static void count(long n) { assert(atomic_load(&anon_page_count) == n); }
static void clean(void) {
    count(0);
    if (live_bytes) fprintf(stderr, "leaked spans: maps=%u unmaps=%u live=%zu\n", maps, unmaps, live_bytes);
    assert(live_bytes == 0 && maps == unmaps);
}
static bool same_reservation(struct mem *mem, page_t page, void *ctx) {
    return mem_find_reservation(mem, page) == ctx;
}
static pages_t cluster(page_t page, bool predicate) {
    pages_t committed = 999;
    void *ctx = predicate ? mem_find_reservation(current->mem, page) : NULL;
    write_wrlock(&current->mem->lock);
    int rc = pt_map_cluster(current->mem, page, P_READ | P_WRITE,
        predicate ? same_reservation : NULL, ctx, &committed);
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
    real_page_size = span; tracking = true;
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
    } else {
        assert(width == 4);
        bool data = !strncmp(mode, "data", 4);
        bool twice = strstr(mode, "double") != NULL;
        assert(data || !strncmp(mode, "mmap", 4));
        if (data) fail_objects = twice ? 2 : 1; else fail_maps = twice ? 2 : 1;
        pages_t committed = 999;
        int rc = pt_map_cluster(current->mem, 0x100, P_READ | P_WRITE, NULL, NULL, &committed);
        if (twice) { assert(rc == _ENOMEM && committed == 0); count(0); }
        else {
            assert(rc == 0 && committed == 1); count(1);
            assert(mem_pt(current->mem, 0x101) == NULL);
            assert(pt_unmap(current->mem, 0x100, 1) == 0);
        }
        assert(!fail_maps && !fail_objects);
    }
    clean();
    printf("upstream-real-cluster-ok span=%zu mode=%s maps=%u unmaps=%u\n", span, mode, maps, unmaps);
    return 0;
}

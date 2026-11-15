#ifndef ISH_MEMORY_CLUSTER_H
#define ISH_MEMORY_CLUSTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Pure bounded selection contract, not an allocator or an enable switch.
// The caller's predicate must prove every page is unmapped, belongs to the SAME
// reservation/stack owner and has compatible permissions. Revalidate under the
// memory write lock before allocation; allocation failure retries one page.
// No current fault path enables clustering. In particular 4KiB hosts return 1.
typedef bool (*ish_cluster_compatible)(uint64_t page, void *context);
static inline unsigned ish_memory_cluster_span(uint64_t fault, uint64_t low,
        uint64_t high, size_t host_bytes, ish_cluster_compatible compatible,
        void *context, uint64_t *start) {
    *start = fault;
    if (!compatible || host_bytes % 4096) return 1;
    size_t width = host_bytes / 4096;
    if (width <= 1 || width > 4 || (width & (width - 1))) return 1;
    uint64_t base = fault & ~((uint64_t)width - 1);
    if (base < low || base > UINT64_MAX - width || base + width > high) return 1;
    for (unsigned i = 0; i < width; i++)
        if (!compatible(base + i, context)) return 1;
    *start = base;
    return (unsigned)width;
}
#endif

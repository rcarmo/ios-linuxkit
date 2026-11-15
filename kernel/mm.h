#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include <stdatomic.h>
#include "kernel/memory.h"
#include "misc.h"

// Logical committed anonymous guest-page ceiling across all processes.
// Not host RSS, a physical-memory guarantee, or iOS jetsam protection.
// 0 = no logical limit; pressure admission is independently opt-in.
// PROT_NONE reservations are not charged. Default: 1048576 * 4KiB = 4GiB.
#ifndef ANON_MMAP_LIMIT_PAGES
#define ANON_MMAP_LIMIT_PAGES 1048576
#endif

#if ANON_MMAP_LIMIT_PAGES > 0
extern _Atomic long anon_page_count;
// Logical committed anonymous guest pages, not host RSS/physical footprint.
bool anon_pages_reserve(long pages);
void anon_pages_unreserve(long pages);
#endif
static inline bool anon_page_is_charged(unsigned flags) {
    return (flags & P_ANONYMOUS) && (flags & P_RWX);
}

// uses mem.lock instead of having a lock of its own
struct mm {
    atomic_uint refcount;
    struct mem mem;

    addr_t vdso; // immutable
    addr_t start_brk; // immutable
    addr_t brk;

    // crap for procfs
    addr_t argv_start;
    addr_t argv_end;
    addr_t env_start;
    addr_t env_end;
    addr_t auxv_start;
    addr_t auxv_end;
    addr_t stack_start;
    struct fd *exefile;
};

// Create a new address space
struct mm *mm_new(void);
// Clone (COW) the address space
struct mm *mm_copy(struct mm *mm);
// Increment the refcount
void mm_retain(struct mm *mem);
// Decrement the refcount, destroy everything in the space if 0
void mm_release(struct mm *mem);

#endif

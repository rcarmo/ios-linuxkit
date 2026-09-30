#ifndef ISH_MEMORY_POLICY_H
#define ISH_MEMORY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

// A sampler obtains this BEFORE measuring available memory. Only allocations
// completed by that point may be reconciled by its published sample.
struct ish_memory_capture {
    uint64_t generation;
    uint64_t completed_bytes;
};
struct ish_memory_sample {
    uint64_t allowance_bytes;
    uint64_t available_bytes;
    uint64_t monotonic_ms;
    bool critical;
    struct ish_memory_capture capture;
};
// Caller-owned transaction; initialise to zero, finish exactly once. It is not
// kept in a global list and is never used for logical map ownership.
struct ish_memory_ticket {
    uint64_t bytes;
    long pages;
    bool active;
};
// Disabled by default. Refuse resets/toggles while a transaction is outstanding.
bool ish_memory_policy_enable(bool enabled);
struct ish_memory_capture ish_memory_policy_capture(void);
bool ish_memory_policy_update(struct ish_memory_sample sample);
bool ish_memory_policy_snapshot(struct ish_memory_sample *sample, bool *braked);
struct ish_memory_budget {
    uint64_t pending_bytes;
    uint64_t unsettled_bytes;
};
struct ish_memory_budget ish_memory_policy_budget(void);

// Reserve both logical guest pages and conservative host-allocation span bytes.
// Finish commits the debt or refunds both debt and the reserved logical pages.
bool ish_memory_reserve_ticket(long logical_pages, uint64_t allocation_bytes,
                               struct ish_memory_ticket *ticket);
bool anon_pages_reserve_ticket(long pages, struct ish_memory_ticket *ticket);
void anon_pages_finish_ticket(struct ish_memory_ticket *ticket, bool committed);

#endif

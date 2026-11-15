#ifndef ISH_MEMORY_POLICY_H
#define ISH_MEMORY_POLICY_H
#include <stdbool.h>
#include <stdint.h>

// Platform-neutral, synchronous admission. Disabled by default. Callers own
// tickets exclusively and finish them exactly once, after installation/cleanup.
// Guest logical-page accounting is independent of host-byte admission.
struct ish_memory_capture { uint64_t generation, monotonic_ms; };
struct ish_memory_sample {
    struct ish_memory_capture capture;
    uint64_t allowance_bytes, available_bytes;
    bool critical;
};
struct ish_memory_ticket { uint64_t bytes; long pages; bool active; };
struct ish_memory_budget { uint64_t pending_bytes, retained_bytes, transactions; };

// Changing mode invalidates the sample, but NEVER erases retained backing debt.
// Refused with active transactions. The caller must provide fresh samples after
// enabling; no platform adapter or worker is implicitly started.
bool ish_memory_policy_enable(bool enabled);
struct ish_memory_capture ish_memory_policy_capture(void);
bool ish_memory_policy_update(struct ish_memory_sample sample);
void ish_memory_policy_invalidate(void);
// An obsolete sampler must not erase a newer capture's accepted sample.
void ish_memory_policy_invalidate_capture(struct ish_memory_capture capture);
bool ish_memory_policy_snapshot(struct ish_memory_sample *sample, bool *braked);
struct ish_memory_budget ish_memory_policy_budget(void);

// bytes is rounded to the real host page size. Pending claims remain charged
// through rollback cleanup. Success returns retained debt to attach to backing.
// A sample DOES NOT settle successful mmap debt: backing may not be resident.
// Release returned debt only when that backing is actually destroyed. This
// deliberately double-counts some resident memory rather than guessing residency.
bool ish_memory_reserve_ticket(long pages, uint64_t bytes, struct ish_memory_ticket *ticket);
bool anon_pages_reserve_ticket(long pages, struct ish_memory_ticket *ticket);
uint64_t anon_pages_finish_ticket(struct ish_memory_ticket *ticket, bool committed);
// Cleanup failed: refund logical pages, but retain host debt without an owner.
// No future sample or mode toggle may erase this quarantined process-lifetime debt.
void ish_memory_abandon_ticket(struct ish_memory_ticket *ticket);
void ish_memory_policy_release(uint64_t bytes);
#endif

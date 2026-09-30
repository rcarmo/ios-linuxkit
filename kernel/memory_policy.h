#ifndef ISH_MEMORY_POLICY_H
#define ISH_MEMORY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

// Experimental additional admission brake. Disabled by default; never replaces
// the logical guest-page accounting ceiling. No platform sampler is wired yet.
struct ish_memory_sample {
    uint64_t allowance_bytes;
    uint64_t available_bytes;
    uint64_t monotonic_ms;
    bool critical;
};
void ish_memory_policy_enable(bool enabled);
// Reject malformed/out-of-order samples without modifying the accepted sample.
bool ish_memory_policy_update(struct ish_memory_sample sample);
// Coherent copy, useful for diagnostics/tests. Returns whether policy is enabled.
bool ish_memory_policy_snapshot(struct ish_memory_sample *sample, bool *braked);

#endif

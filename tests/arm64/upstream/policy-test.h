#ifndef UPSTREAM_POLICY_TEST_H
#define UPSTREAM_POLICY_TEST_H
#include <assert.h>
#include <stdlib.h>
#include "kernel/memory_policy.h"
// Explicit test-only opt-in. No platform sampler, host settings or hidden worker.
static inline void upstream_policy_feed(void) {
    struct ish_memory_sample sample = {ish_memory_policy_capture(), 1ULL << 30, 1ULL << 30, false};
    assert(ish_memory_policy_update(sample));
}
static inline void upstream_policy_start(void) {
    if (getenv("ISH_TEST_MEMORY_POLICY")) {
        assert(ish_memory_policy_enable(true));
        upstream_policy_feed();
    }
}
static inline void upstream_policy_empty(void) {
    if (getenv("ISH_TEST_MEMORY_POLICY")) {
        struct ish_memory_budget budget = ish_memory_policy_budget();
        assert(!budget.pending_bytes && !budget.retained_bytes && !budget.transactions);
    }
}
#endif

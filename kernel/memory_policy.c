#include <assert.h>
#include <limits.h>
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/memory_policy.h"
#include "util/sync.h"
#include "util/timer.h"

static lock_t policy_lock = LOCK_INITIALIZER;
static bool enabled, valid, braked;
static struct ish_memory_sample sample;
static struct ish_memory_capture issued;
static uint64_t accepted, pending, retained, transactions;
static uint64_t now_ms(void) {
    struct timespec t = timespec_now(CLOCK_MONOTONIC);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
bool ish_memory_policy_enable(bool value) {
    lock(&policy_lock);
    bool ok = !transactions;
    if (ok) {
        enabled = value;
        valid = false;
        braked = value;
        accepted = issued.generation;
        sample = (struct ish_memory_sample){0};
    }
    unlock(&policy_lock);
    return ok;
}
struct ish_memory_capture ish_memory_policy_capture(void) {
    lock(&policy_lock);
    struct ish_memory_capture c = {0};
    if (issued.generation != UINT64_MAX) {
        issued = (struct ish_memory_capture){issued.generation + 1, now_ms()};
        c = issued;
    }
    unlock(&policy_lock);
    return c;
}
bool ish_memory_policy_update(struct ish_memory_sample value) {
    lock(&policy_lock);
    uint64_t now = now_ms();
    bool ok = enabled && value.allowance_bytes &&
        value.available_bytes <= value.allowance_bytes &&
        value.capture.generation > accepted &&
        value.capture.generation == issued.generation &&
        value.capture.monotonic_ms == issued.monotonic_ms &&
        value.capture.monotonic_ms && now >= value.capture.monotonic_ms &&
        now - value.capture.monotonic_ms <= 2000;
    if (ok) {
        unsigned percent = braked ? 15 : 10;
        uint64_t threshold = value.allowance_bytes / 100 * percent +
            value.allowance_bytes % 100 * percent / 100;
        braked = value.critical || value.available_bytes < threshold;
        sample = value;
        accepted = value.capture.generation;
        valid = true;
    }
    unlock(&policy_lock);
    return ok;
}
void ish_memory_policy_invalidate(void) {
    lock(&policy_lock);
    valid = false;
    braked = enabled;
    accepted = issued.generation;
    unlock(&policy_lock);
}
void ish_memory_policy_invalidate_capture(struct ish_memory_capture capture) {
    lock(&policy_lock);
    if (capture.generation == issued.generation && capture.monotonic_ms == issued.monotonic_ms) {
        valid = false;
        braked = enabled;
        accepted = issued.generation;
    }
    unlock(&policy_lock);
}
bool ish_memory_policy_snapshot(struct ish_memory_sample *out, bool *out_braked) {
    lock(&policy_lock);
    if (out) *out = sample;
    if (out_braked) *out_braked = braked;
    bool result = enabled;
    unlock(&policy_lock);
    return result;
}
struct ish_memory_budget ish_memory_policy_budget(void) {
    lock(&policy_lock);
    struct ish_memory_budget b = {pending, retained, transactions};
    unlock(&policy_lock);
    return b;
}
bool ish_memory_reserve_ticket(long pages, uint64_t bytes, struct ish_memory_ticket *ticket) {
    if (!ticket || ticket->active || pages < 0) return false;
    uint64_t span = real_page_size;
    if (!span || bytes > UINT64_MAX - (span - 1)) return false;
    bytes = (bytes + span - 1) / span * span;
    lock(&policy_lock);
    if (!enabled) bytes = 0;
    bool ok = transactions != UINT64_MAX && pending <= UINT64_MAX - bytes &&
        retained <= UINT64_MAX - pending - bytes;
    if (ok && bytes && enabled) {
        uint64_t now = now_ms(), available = sample.available_bytes;
        ok = valid && !braked && now >= sample.capture.monotonic_ms &&
            now - sample.capture.monotonic_ms <= 2000 &&
            retained <= available && pending <= available - retained &&
            bytes <= available - retained - pending;
    }
#if ANON_MMAP_LIMIT_PAGES > 0
    if (ok && pages) ok = anon_pages_reserve(pages);
#endif
    if (ok) {
        pending += bytes;
        transactions++;
        *ticket = (struct ish_memory_ticket){bytes, pages, true};
    }
    unlock(&policy_lock);
    return ok;
}
bool anon_pages_reserve_ticket(long pages, struct ish_memory_ticket *ticket) {
    if (pages < 0 || (uint64_t)pages > UINT64_MAX / PAGE_SIZE) return false;
    return ish_memory_reserve_ticket(pages, (uint64_t)pages * PAGE_SIZE, ticket);
}
static uint64_t finish_ticket(struct ish_memory_ticket *ticket, bool committed, bool orphaned) {
    lock(&policy_lock);
    assert(ticket && ticket->active && transactions && pending >= ticket->bytes);
    uint64_t debt = committed || orphaned ? ticket->bytes : 0;
    pending -= ticket->bytes;
    retained += debt;
    transactions--;
#if ANON_MMAP_LIMIT_PAGES > 0
    if (!committed) anon_pages_unreserve(ticket->pages);
#endif
    *ticket = (struct ish_memory_ticket){0};
    unlock(&policy_lock);
    return debt;
}
uint64_t anon_pages_finish_ticket(struct ish_memory_ticket *ticket, bool committed) {
    return finish_ticket(ticket, committed, false);
}
void ish_memory_abandon_ticket(struct ish_memory_ticket *ticket) {
    finish_ticket(ticket, false, true);
}
void ish_memory_policy_release(uint64_t bytes) {
    if (!bytes) return;
    lock(&policy_lock);
    assert(retained >= bytes);
    retained -= bytes;
    unlock(&policy_lock);
}

// Select real Darwin no-emitter conventions; do not borrow recording headers.
#include "asbestos/guest-arm64/jit.c"
#include <assert.h>

static void arena_capacity_probe(void) {
    assert(!region_alloc(16) && !atomic_load(&region_exhausted));
    region = mmap(NULL, REGION_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(region != MAP_FAILED);
    assert(!region_alloc(SIZE_MAX) && !atomic_load(&region_used));
    atomic_store(&region_used, REGION_SIZE - 16);
    assert(!region_alloc(32) && atomic_load(&region_exhausted));
    assert(atomic_load(&region_used) == REGION_SIZE - 16);
    assert(region_alloc(16) == region + REGION_SIZE - 16);
    assert(!region_alloc(1) && atomic_load(&region_used) == REGION_SIZE);
    assert(munmap(region, REGION_SIZE) == 0);
    region = NULL;
    atomic_store(&region_used, 0);
    atomic_store(&region_exhausted, false);
}

int main(void) {
    arena_capacity_probe();
    struct jit_layout before, prepared, after;
    assert(jit_layout_read(&before) == 0 && !before.ready && !before.abi);
    assert(jit_aot_prepare_layout() == 0);
    assert(jit_layout_read(&prepared) == 0);
    assert(prepared.ready && prepared.pic && !prepared.emission_compiled);
    assert(prepared.abi == jit_abi() && prepared.prologue_words == (unsigned)prologue_words);
    assert(prepared.entry_off == entry_off() && prepared.n_pinned == (unsigned)n_pinned);
    assert(!region && !jit_on && !aot_nregistered && !aot_nimages);
    // Subsequent environment changes cannot change a published contract.
    assert(setenv("ISH_JIT_PIN", pin_on ? "0" : "1", 1) == 0);
    assert(jit_aot_prepare_layout() == 0);
    assert(setenv("ISH_JIT", "0", 1) == 0);
    assert(jit_units_begin() == NULL);
    assert(jit_layout_read(&after) == 0 && !memcmp(&prepared, &after, sizeof(after)));
    assert(!region && !jit_on && !aot_nregistered && !aot_nimages);
    char json[2048];
    assert(jit_layout_describe(json, sizeof(json)) > 0);
    fputs(json, stdout);
}

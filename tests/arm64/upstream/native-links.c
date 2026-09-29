// Actual integration sources: a refused PIC edge still owns a code pointer.
// Exercise repeated refusal, replacement and invalidation without timing races.
#include "asbestos/guest-arm64/jit.c"
#undef DEFAULT_CHANNEL
#include "asbestos/asbestos.c"

static struct fiber_block *fixture(addr_t pc) {
    struct gen_state s;
    gen_start(pc, &s);
    assert(!s.oom);
    gen_end(&s);
    return s.block;
}

int main(void) {
    pic_on = true;
    struct asbestos *a = asbestos_new(NULL);
    struct fiber_block *from = fixture(0x10000);
    struct fiber_block *old = fixture(0x10040);
    struct fiber_block *to = fixture(0x10040);
    struct jit_ctx *ctx = ctx_get(a, 0, 0x10000, NULL);
    assert(ctx);
    from->jit_ctx = ctx;
    slot_set(ctx, 0, from, 0x10000, 0x10000);
    // An immutable link naming another native target. These gadget-only
    // replacement blocks must use the context alternate instead.
    uint64_t words[2] = {0, 4};
    from->native_link[0] = (uint32_t *)words + 1;
    from->jump_ip[0] = &from->code[0];
    from->old_jump_ip[0] = ARM64_FAKE_IP_TAG | to->addr;
    from->code[0] = from->old_jump_ip[0];
    from->jump_ip_is_fake[0] = true;
    assert(!fiber_prechain_patch_slot(from, 0, old));
    assert(ctx_far(ctx, 0)->alt[0] == (uintptr_t)old->code);
    assert(list_size(&old->jumps_from[0]) == 1);
    for (int i = 0; i < 100; i++) {
        assert(!fiber_prechain_patch_slot(from, 0, to));
        assert(list_size(&to->jumps_from[0]) == 1);
    }
    assert(list_empty(&old->jumps_from[0]));
    fiber_block_disconnect(NULL, old);
    assert(ctx_far(ctx, 0)->alt[0] == (uintptr_t)to->code);
    fiber_block_disconnect(NULL, to);
    assert(ctx_far(ctx, 0)->alt[0] == 0);
    assert(from->code[0] == from->old_jump_ip[0]);
    assert(from->jump_ip_is_fake[0]);
    jit_block_free(from);
    free(from); free(old); free(to);
    asbestos_free(a);
    puts("native-refused-link-invalidation-ok repeats=100");
}

// Execute the real decoder and gadgets across a refused write, then retry.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asbestos/gen.h"
#include "asbestos/frame.h"
#include "emu/interrupt.h"

extern int fiber_enter(struct fiber_block *, struct fiber_frame *, struct tlb *);
static uint32_t instructions[PAGE_SIZE / 4] = {
    0xf90000a0, // str x0,[x5]: a preceding memory checkpoint
    0x91010063, // add x3,x3,#64: must not be replayed
    0xd50b7423, // dc zva,x3
};
static unsigned char first[PAGE_SIZE], target[PAGE_SIZE];
static bool writable;
static unsigned faults;

static void *translate(struct mmu *mmu, addr_t addr, int type) {
    (void) mmu;
    if (PAGE(addr) == 1) return (char *) instructions + PGOFFSET(addr);
    if (PAGE(addr) == 4) return first + PGOFFSET(addr);
    if (PAGE(addr) == 5) {
        if (type == MEM_WRITE && !writable) { faults++; return NULL; }
        return target + PGOFFSET(addr);
    }
    return NULL;
}
static void *nofault(struct mmu *mmu, addr_t addr) {
    if (PAGE(addr) == 5 && !writable) return NULL;
    return translate(mmu, addr, MEM_WRITE);
}
static struct fiber_block *generate(addr_t pc, struct tlb *tlb) {
    struct gen_state state;
    gen_start(pc, &state);
    while (state.ip < 0x100c) assert(gen_step(&state, tlb) == 1);
    gen_exit(&state);
    gen_end(&state);
    assert(!state.oom);
    return state.block;
}
int main(void) {
    struct mmu_ops ops = { .translate = translate, .translate_write_nofault = nofault };
    struct mmu mmu = { .ops = &ops };
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(tlb);
    tlb_refresh(tlb, &mmu);
    for (unsigned offset = 0; offset < 64; offset++) {
        struct fiber_frame frame = {0};
        frame.cpu.regs[0] = 0x1122334455667788;
        frame.cpu.regs[3] = 0x4fc0 + offset;
        frame.cpu.regs[5] = 0x4000;
        frame.cpu.nzcv = 0xa0000000;
        memset(frame.cpu.fp, 0x37, sizeof(frame.cpu.fp));
        memset(first, 0x5a, sizeof(first));
        memset(target, 0x5a, sizeof(target));
        writable = false;
        faults = 0;
        tlb_flush(tlb);
        struct fiber_block *block = generate(0x1000, tlb);
        assert(fiber_enter(block, &frame, tlb) == INT_GPF);
        assert(faults == 1 && frame.cpu.pc == 0x1008);
        assert(frame.jit_saved_pc == 0x1008);
        assert(frame.cpu.segfault_addr == 0x5000 && frame.cpu.segfault_was_write);
        assert(frame.cpu.regs[3] == 0x5000 + offset);
        assert(!memcmp(first, &frame.cpu.regs[0], 8));
        for (unsigned i = 0; i < sizeof(target); i++) assert(target[i] == 0x5a);
        free(block);

        writable = true;
        block = generate(frame.cpu.pc, tlb);
        assert(fiber_enter(block, &frame, tlb) == INT_NONE);
        assert(frame.cpu.pc == 0x100c && faults == 1);
        assert(frame.cpu.regs[3] == 0x5000 + offset && frame.cpu.nzcv == 0xa0000000);
        for (unsigned i = 0; i < sizeof(target); i++) assert(target[i] == (i < 64 ? 0 : 0x5a));
        for (unsigned i = 0; i < sizeof(frame.cpu.fp); i++)
            assert(((unsigned char *) frame.cpu.fp)[i] == 0x37);
        free(block);
    }
    free(tlb);
    puts("dczva-precise-retry-ok offsets=64");
}

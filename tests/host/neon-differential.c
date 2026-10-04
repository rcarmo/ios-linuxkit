#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include "asbestos/gen.h"
#include "asbestos/frame.h"
#include "emu/interrupt.h"

extern int fiber_enter(struct fiber_block *, struct fiber_frame *, struct tlb *);
extern uint64_t neon_reference(void *, void *);
static uint32_t guest[PAGE_SIZE / 4];
static void *translate(struct mmu *mmu, addr_t addr, int type) {
    (void) mmu; (void) type;
    return PAGE(addr) == 1 ? (char *) guest + PGOFFSET(addr) : NULL;
}
static uint64_t random_state = 42;
static uint64_t random_word(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}
int main(int argc, char **argv) {
    assert(argc == 1 || argc == 2);
    FILE *input = argc == 2 ? fopen(argv[1], "rb") : NULL;
    assert(argc == 1 || input);
    uint32_t shift_ops[64]; unsigned count = 0, index = 0;
    for (unsigned q = 0; q < 2; q++) for (unsigned size = 0; size < 4; size++) {
        if (!q && size == 3) continue;
        for (unsigned alias = 0; alias < 4; alias++) {
            unsigned rn = alias == 1 ? 0 : 1, rm = alias == 2 ? 0 : alias == 3 ? rn : 2;
            shift_ops[count++] = 0x2e204400 | (q << 30) | (size << 22) | (rm << 16) | (rn << 5);
        }
    }
    shift_ops[count++] = 0x6ee14400;
    shift_ops[count++] = 0x2ea04420;
    struct mmu_ops ops = { .translate = translate };
    struct mmu mmu = { .ops = &ops };
    struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
    tlb_refresh(tlb, &mmu);
    uint32_t *code = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0); assert(code != MAP_FAILED);
    unsigned cases = 0, failures = 0;
    uint32_t opcode;
    while (input ? fread(&opcode, 4, 1, input) == 1 :
            (index < count && (opcode = shift_ops[index++], 1))) {
        bool gpr_source = (opcode & 0xbfe0fc00) == 0x0e000c00 ||
                          (opcode & 0xffe0fc00) == 0x4e001c00;
        bool gpr_dest = (opcode & 0x9fe0fc00) == 0x0e003c00 ||
                        (opcode & 0x9fe0fc00) == 0x0e002c00;
        if (gpr_source) opcode &= ~(31u << 5);
        if (gpr_dest) opcode &= ~31u;
        guest[0] = opcode;
        struct gen_state state; gen_start(0x1000, &state);
        if (gen_step(&state, tlb) != 1) { free(state.block); continue; }
        gen_exit(&state); gen_end(&state);
        pthread_jit_write_protect_np(0);
        code[0] = 0x58000080; // ldr x0,+16: general-register copy input
        code[1] = opcode; code[2] = 0xd65f03c0; code[3] = 0;
        pthread_jit_write_protect_np(1);
        for (unsigned iteration = 0; iteration < 100; iteration++) {
            struct fiber_frame frame = {0};
            for (unsigned i = 0; i < sizeof(frame.cpu.fp) / 8; i++)
                ((uint64_t *) frame.cpu.fp)[i] = random_word();
            unsigned char expected[sizeof(frame.cpu.fp)];
            memcpy(expected, frame.cpu.fp, sizeof(expected));
            frame.cpu.regs[0] = random_word();
            frame.cpu.nzcv = (random_word() & 15) << 28;
            uint32_t nzcv = frame.cpu.nzcv;
            pthread_jit_write_protect_np(0);
            memcpy(code + 4, &frame.cpu.regs[0], 8);
            __builtin___clear_cache((char *) code, (char *) (code + 6));
            pthread_jit_write_protect_np(1);
            uint64_t expected_gpr = neon_reference(code, expected);
            assert(fiber_enter(state.block, &frame, tlb) == INT_NONE);
            cases++;
            if (memcmp(expected, frame.cpu.fp, sizeof(expected)) ||
                    (gpr_dest && frame.cpu.regs[0] != expected_gpr) || frame.cpu.nzcv != nzcv) {
                printf("NEON_MISMATCH opcode=%08x iteration=%u\n", opcode, iteration);
                failures++;
                break;
            }
        }
        free(state.block);
    }
    printf("NEON_DIFFERENTIAL cases=%u failures=%u\n", cases, failures);
    if (input) fclose(input);
    free(tlb); munmap(code, 16384);
    return failures != 0;
}

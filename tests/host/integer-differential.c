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
extern uint64_t integer_reference(void *, struct cpu_state *, unsigned);
#include "tests/arm64/native-aot/differential-ops.h"
_Static_assert(offsetof(struct cpu_state, regs) == 16, "native reference register ABI");
static uint32_t guest[PAGE_SIZE / 4];
static void *translate(struct mmu *mmu, addr_t addr, int type) {
    (void) mmu; (void) type;
    return PAGE(addr) == 1 ? (char *) guest + PGOFFSET(addr) : NULL;
}
static uint64_t rng = 42;
static uint64_t random_word(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
static unsigned normalize(uint32_t *w, unsigned shift, int *map, int *next) {
    // Keep aliases and XZR while limiting native operands to caller-saved X0-X7.
    unsigned r = (*w >> shift) & 31;
    if (r == 31) return r;
    if (map[r] < 0) map[r] = (*next)++;
    *w = (*w & ~(31u << shift)) | (map[r] << shift);
    return r;
}
int main(int argc, char **argv) {
    assert(argc == 1 || argc == 2);
    FILE *input = argc == 2 ? fopen(argv[1], "rb") : NULL;
    assert(argc == 1 || input);
    struct mmu_ops ops = { .translate = translate };
    struct mmu mmu = { .ops = &ops };
    struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
    tlb_refresh(tlb, &mmu);
    uint32_t *code = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0); assert(code != MAP_FAILED);
    unsigned cases = 0, failures = 0, index = 0; uint32_t original;
    while (input ? fread(&original, 4, 1, input) == 1 :
            (index < sizeof(diff_ops) / sizeof(*diff_ops) && (original = diff_ops[index++], 1))) {
        uint32_t w = original;
        unsigned group = (w >> 25) & 15;
        int map[32], next = 0; memset(map, -1, sizeof(map));
        if (group == 5 || group == 13) {
            unsigned op1 = (w >> 28) & 1, op2 = (w >> 21) & 15;
            if (!op1 && (op2 & 9) == 9 && (((w >> 5) & 31) == 31 || (w & 31) == 31)) continue;
            bool compare = op1 && (op2 & 14) == 2;
            if (!compare) normalize(&w, 0, map, &next);
            normalize(&w, 5, map, &next);
            if (!(op1 && op2 == 6 && ((w >> 30) & 1)) && !(compare && (w & (1u << 11))))
                normalize(&w, 16, map, &next);
            if (op1 && (op2 & 8)) normalize(&w, 10, map, &next);
        } else if (group == 8 || group == 9) {
            unsigned op = (w >> 23) & 7;
            if (op < 2) continue; // ADR/ADRP have PC-dependent results.
            if (op == 2 && (((w >> 5) & 31) == 31 || (w & 31) == 31)) continue;
            if (op == 4 && (w & 31) == 31 && ((w >> 29) & 3) != 3) continue;
            normalize(&w, 0, map, &next);
            if (op != 5) normalize(&w, 5, map, &next); // Wide moves have no Rn.
            if (op == 7) normalize(&w, 16, map, &next); // EXTR has two sources.
        } else continue;
        guest[0] = w; guest[1] = 0; // Do not allow instruction fusion.
        struct gen_state state; gen_start(0x1000, &state);
        if (gen_step(&state, tlb) != 1) { free(state.block); continue; }
        gen_exit(&state); gen_end(&state);
        pthread_jit_write_protect_np(0);
        code[0] = w; code[1] = 0xd65f03c0;
        __builtin___clear_cache((char *) code, (char *) (code + 2));
        pthread_jit_write_protect_np(1);
        for (unsigned iteration = 0; iteration < 100; iteration++) {
            struct fiber_frame frame = {0};
            for (int i = 0; i < 8; i++) frame.cpu.regs[i] = random_word();
            frame.cpu.nzcv = (random_word() & 15) << 28;
            struct cpu_state expected = frame.cpu;
            expected.nzcv = integer_reference(code, &expected, offsetof(struct cpu_state, nzcv));
            assert(fiber_enter(state.block, &frame, tlb) == INT_NONE);
            cases++;
            if (memcmp(expected.regs, frame.cpu.regs, 8 * 8) || expected.nzcv != frame.cpu.nzcv) {
                printf("INTEGER_MISMATCH original=%08x normalized=%08x iteration=%u nzcv=%08x/%08x\n",
                    original, w, iteration, expected.nzcv, frame.cpu.nzcv);
                for (int i = 0; i < 8; i++) if (expected.regs[i] != frame.cpu.regs[i])
                    printf(" x%d=%016llx/%016llx\n", i, (unsigned long long) expected.regs[i],
                        (unsigned long long) frame.cpu.regs[i]);
                failures++; break;
            }
        }
        free(state.block);
    }
    printf("INTEGER_DIFFERENTIAL cases=%u failures=%u\n", cases, failures);
    if (input) fclose(input);
    free(tlb); munmap(code, 16384);
    return failures != 0;
}

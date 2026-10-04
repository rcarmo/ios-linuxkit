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
static _Alignas(16) unsigned char memory[262144], native_memory[262144];
static void *translate(struct mmu *mmu, addr_t addr, int type) {
    (void) mmu; (void) type;
    if (PAGE(addr) == 1) return (char *) guest + PGOFFSET(addr);
    if (addr >= 0x20000 && addr < 0x60000) return memory + addr - 0x20000;
    return NULL;
}
static uint64_t rng = 42;
static uint64_t random_word(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
int main(int argc, char **argv) {
    assert(argc == 1 || argc == 2);
    FILE *input = argc == 2 ? fopen(argv[1], "rb") : NULL;
    assert(argc == 1 || input);
    uint32_t lane_ops[2048]; unsigned count = 0, index = 0;
    for (unsigned load = 0; load < 2; load++) for (unsigned n = 1; n <= 4; n++)
        for (unsigned size = 0; size < 4; size++) for (unsigned lane = 0; lane < (16u >> size); lane++)
            for (unsigned post = 0; post < 3; post++) for (unsigned rt = 8; rt <= 30; rt += 22) {
                unsigned q, s, bits;
                if (size == 0) { q = lane >> 3; s = (lane >> 2) & 1; bits = lane & 3; }
                else if (size == 1) { q = lane >> 2; s = (lane >> 1) & 1; bits = (lane & 1) << 1; }
                else if (size == 2) { q = lane >> 1; s = lane & 1; bits = 0; }
                else { q = lane; s = 0; bits = 1; }
                unsigned opcode = (size < 3 ? size * 2 : 4) | (n >= 3);
                assert(count < sizeof(lane_ops) / sizeof(*lane_ops));
                lane_ops[count++] = 0x0d000000 | (q << 30) | (load << 22) |
                    ((n % 2 == 0) << 21) | (opcode << 13) | (s << 12) | (bits << 10) | rt |
                    (post ? (1u << 23) | ((post == 1 ? 31 : 1) << 16) : 0);
            }
    struct mmu_ops ops = { .translate = translate };
    struct mmu mmu = { .ops = &ops };
    struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
    tlb_refresh(tlb, &mmu);
    uint32_t *code = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0); assert(code != MAP_FAILED);
    unsigned cases = 0, failures = 0; uint32_t original;
    while (input ? fread(&original, 4, 1, input) == 1 :
            (index < count && (original = lane_ops[index++], 1))) {
        unsigned group = (original >> 25) & 15;
        if ((group & 5) != 4 || !(original & 0x04000000)) continue;
        if ((original & 0x3b000000) == 0x18000000) continue;
        uint32_t w = original & ~(31u << 5); // Base always X0, never host SP.
        bool structure = (w & 0xbe000000) == 0x0c000000;
        if ((structure && (w & 0x00800000) && ((w >> 16) & 31) != 31) ||
            (!structure && (w & 0x3b200c00) == 0x38200800))
            w = (w & ~(31u << 16)) | (1u << 16);
        guest[0] = w; guest[1] = 0;
        struct gen_state state; gen_start(0x1000, &state);
        if (gen_step(&state, tlb) != 1) { free(state.block); continue; }
        gen_exit(&state); gen_end(&state);
        pthread_jit_write_protect_np(0);
        code[0] = 0x58000080; code[1] = 0x580000a1;
        code[2] = w; code[3] = 0xd65f03c0;
        pthread_jit_write_protect_np(1);
        unsigned offsets[] = {0, 1, 7, 15, 31, 4063, 4095};
        for (unsigned iteration = 0; iteration < 7; iteration++) {
            struct fiber_frame frame = {0};
            for (unsigned i = 0; i < sizeof(frame.cpu.fp) / 8; i++)
                ((uint64_t *) frame.cpu.fp)[i] = random_word();
            for (unsigned i = 0; i < sizeof(memory) / 8; i++)
                ((uint64_t *) memory)[i] = random_word();
            memcpy(native_memory, memory, sizeof(memory));
            unsigned char expected[sizeof(frame.cpu.fp)];
            memcpy(expected, frame.cpu.fp, sizeof(expected));
            unsigned offset = 16384 + offsets[iteration];
            frame.cpu.regs[0] = 0x20000 + offset;
            frame.cpu.regs[1] = 16;
            uint64_t native_base = (uintptr_t) native_memory + offset, index = 16;
            pthread_jit_write_protect_np(0);
            memcpy(code + 4, &native_base, 8); memcpy(code + 6, &index, 8);
            __builtin___clear_cache((char *) code, (char *) (code + 8));
            pthread_jit_write_protect_np(1);
            uint64_t expected_base = neon_reference(code, expected) - (uintptr_t) native_memory + 0x20000;
            int interrupt = fiber_enter(state.block, &frame, tlb);
            if (interrupt != INT_NONE) {
                printf("SIMD_MEMORY_FAULT original=%08x normalized=%08x offset=%u interrupt=%d\n", original, w, offset, interrupt);
                failures++; break;
            }
            cases++;
            if (memcmp(expected, frame.cpu.fp, sizeof(expected)) ||
                memcmp(memory, native_memory, sizeof(memory)) || frame.cpu.regs[0] != expected_base) {
                printf("SIMD_MEMORY_MISMATCH original=%08x normalized=%08x offset=%u fp=%d memory=%d base=%llx/%llx\n",
                    original, w, offset, memcmp(expected, frame.cpu.fp, sizeof(expected)),
                    memcmp(memory, native_memory, sizeof(memory)), (unsigned long long) expected_base,
                    (unsigned long long) frame.cpu.regs[0]);
                failures++; break;
            }
        }
        free(state.block);
    }
    printf("SIMD_MEMORY_DIFFERENTIAL cases=%u failures=%u\n", cases, failures);
    if (input) fclose(input);
    free(tlb); munmap(code, 16384);
    return failures != 0;
}

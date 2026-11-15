/* Actual candidate crypto objects, independent scalar model and guarded native
 * oracle. No rootfs or JIT dependency. Exhaust every register alias tuple. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include "cpu-offsets.h"

extern unsigned char ish_host_sha512;
void sha512su0_helper(uint64_t *, uint64_t *);
void sha512su1_helper(uint64_t *, uint64_t *, uint64_t *);
void sha512h_helper(uint64_t *, uint64_t *, uint64_t *);
void sha512h2_helper(uint64_t *, uint64_t *, uint64_t *);
extern void gadget_sha512su0(void), gadget_sha512su1(void);
extern void gadget_sha512h(void), gadget_sha512h2(void);
int sha512_fallback_gadget_run(void (*)(void), void *, uint64_t);
void sha512_fallback_native(unsigned, uint64_t *, const uint64_t *, const uint64_t *);

static uint64_t rotate(uint64_t v, unsigned n) { return (v >> n) | (v << (64 - n)); }
static uint64_t small0(uint64_t v) { return rotate(v, 1) ^ rotate(v, 8) ^ (v >> 7); }
static uint64_t small1(uint64_t v) { return rotate(v, 19) ^ rotate(v, 61) ^ (v >> 6); }
static uint64_t big0(uint64_t v) { return rotate(v, 28) ^ rotate(v, 34) ^ rotate(v, 39); }
static uint64_t big1(uint64_t v) { return rotate(v, 14) ^ rotate(v, 18) ^ rotate(v, 41); }
static uint64_t choose(uint64_t a, uint64_t b, uint64_t c) { return c ^ (a & (b ^ c)); }
static uint64_t majority(uint64_t a, uint64_t b, uint64_t c) { return (a & b) | ((a | b) & c); }
static void model(unsigned op, uint64_t out[2], const uint64_t d[2],
                  const uint64_t n[2], const uint64_t m[2]) {
    switch (op) {
    case 0: out[0] = d[0] + small0(d[1]); out[1] = d[1] + small0(n[0]); break;
    case 1: out[0] = d[0] + small1(n[0]) + m[0]; out[1] = d[1] + small1(n[1]) + m[1]; break;
    case 2:
        out[1] = d[1] + big1(m[1]) + choose(m[1], n[0], n[1]);
        out[0] = d[0] + big1(out[1] + m[0]) + choose(out[1] + m[0], m[1], n[0]); break;
    default:
        out[1] = d[1] + big0(m[0]) + majority(n[0], m[1], m[0]);
        out[0] = d[0] + big0(out[1]) + majority(out[1], m[0], m[1]); break;
    }
}
#ifndef SHA512_GUEST
static void helper(unsigned op, uint64_t *d, uint64_t *n, uint64_t *m) {
    switch (op) {
    case 0: sha512su0_helper(d, n); break;
    case 1: sha512su1_helper(d, n, m); break;
    case 2: sha512h_helper(d, n, m); break;
    default: sha512h2_helper(d, n, m); break;
    }
}
#endif
static uint64_t random_word(void) {
    static uint64_t seed = UINT64_C(0x123456789abcdef);
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed;
}
#ifdef SHA512_GUEST
/* Guest executes each actual ISA opcode; its host must take the software path
 * when forced, irrespective of the HWCAP advertised to the guest. */
int main(void) {
    for (unsigned op = 0; op < 4; op++)
        for (unsigned sample = 0; sample < 256; sample++) {
            uint64_t d[2], n[2], m[2], expected[2];
            for (unsigned i = 0; i < 2; i++) {
                d[i] = random_word(); n[i] = random_word(); m[i] = random_word();
            }
            model(op, expected, d, n, m);
            sha512_fallback_native(op, d, n, m);
            assert(memcmp(d, expected, sizeof(d)) == 0);
        }
    puts("sha512-fallback-guest-ok instructions=1024");
    return 0;
}
#else
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    int native = (getauxval(AT_HWCAP) & HWCAP_SHA512) != 0;
    assert(ish_host_sha512 == (strcmp(argv[1], "forced") == 0 ? 0 : native));
    if (argc == 3) {
        assert(strcmp(argv[1], "forced") == 0 && strcmp(argv[2], "no-native-oracle") == 0);
        native = 0; // Exercise the complete no-hardware test lane on any host.
    }
    /* Partial byte-range overlap (aligned words), exact aliases, zero/all-one,
     * carry-heavy and randomized inputs, always checked even without FEAT. */
    unsigned helpers = 0, gadgets = 0;
    for (unsigned op = 0; op < 4; op++)
        for (unsigned sample = 0; sample < 128; sample++)
            for (unsigned n = 0; n < 5; n++)
                for (unsigned m = 0; m < 5; m++) {
                    uint64_t words[8], before[8], expected[2];
                    for (unsigned i = 0; i < 8; i++)
                        words[i] = sample == 0 ? 0 : sample == 1 ? UINT64_MAX : random_word();
                    memcpy(before, words, sizeof(words));
                    model(op, expected, before + 2, before + n, before + m);
                    if (native) {
                        uint64_t oracle[2]; memcpy(oracle, before + 2, sizeof(oracle));
                        sha512_fallback_native(op, oracle, before + n, before + m);
                        assert(memcmp(oracle, expected, sizeof(expected)) == 0);
                    }
                    helper(op, words + 2, words + n, words + m);
                    memcpy(before + 2, expected, sizeof(expected));
                    assert(memcmp(words, before, sizeof(words)) == 0);
                    helpers++;
                }
    void (*functions[])(void) = {gadget_sha512su0, gadget_sha512su1, gadget_sha512h, gadget_sha512h2};
    for (unsigned op = 0; op < 4; op++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned n = 0; n < 32; n++)
                for (unsigned m = 0; m < 32; m++) {
                    _Alignas(16) unsigned char cpu[1024], expected[1024];
                    uint64_t result[2];
                    for (unsigned i = 0; i < sizeof(cpu) / 8; i++) {
                        uint64_t v = random_word(); memcpy(cpu + i * 8, &v, 8);
                    }
                    memcpy(expected, cpu, sizeof(cpu));
                    uint64_t *fp = (uint64_t *)(cpu + CPU_fp);
                    model(op, result, fp + d * 2, fp + n * 2, fp + m * 2);
                    if (native) {
                        uint64_t oracle[2]; memcpy(oracle, fp + d * 2, sizeof(oracle));
                        sha512_fallback_native(op, oracle, fp + n * 2, fp + m * 2);
                        assert(memcmp(oracle, result, sizeof(result)) == 0);
                    }
                    memcpy(expected + CPU_fp + d * 16, result, sizeof(result));
                    uint64_t operand = d | (n << 8) | (m << 16) | ((uint64_t)(d & 15) << 60);
                    assert(sha512_fallback_gadget_run(functions[op], cpu, operand) == 0);
                    assert(memcmp(cpu, expected, sizeof(cpu)) == 0);
                    gadgets++;
                }
    printf("sha512-fallback-ok helpers=%u gadgets=%u native-oracle=%d\n", helpers, gadgets, native);
    return 0;
}
#endif

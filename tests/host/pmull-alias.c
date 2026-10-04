#include <stdint.h>
#include <stdio.h>
#include <string.h>

void pmull_helper(uint8_t *, uint8_t *, uint8_t *, uint32_t, uint32_t);

static void reference(uint8_t out[16], const uint8_t n[16], const uint8_t m[16], unsigned q, unsigned size) {
    memset(out, 0, 16);
    unsigned bytes = size == 0 ? 1 : 8;
    for (unsigned lane = 0; lane < 8 / bytes; lane++) {
        for (unsigned i = 0; i < bytes * 8; i++) {
            if (!(m[q * 8 + lane * bytes + i / 8] & (1u << (i % 8)))) continue;
            for (unsigned j = 0; j < bytes * 8; j++) {
                if (n[q * 8 + lane * bytes + j / 8] & (1u << (j % 8)))
                    out[lane * bytes * 2 + (i + j) / 8] ^= 1u << ((i + j) % 8);
            }
        }
    }
}

int main(void) {
    for (unsigned size = 0; size <= 3; size += 3) for (unsigned q = 0; q < 2; q++) {
        for (unsigned alias = 0; alias < 4; alias++) for (unsigned seed = 1; seed < 100; seed++) {
            uint8_t n[16], m[16], d[16], expected[16];
            for (unsigned i = 0; i < 16; i++) { n[i] = i * 17 + seed; m[i] = i * 31 + seed * 7; }
            uint8_t *source_m = alias == 3 ? n : m;
            uint8_t *dest = alias == 0 ? d : alias == 2 ? m : n;
            reference(expected, n, source_m, q, size);
            pmull_helper(dest, n, source_m, q, size);
            if (memcmp(expected, dest, 16)) {
                fprintf(stderr, "PMULL mismatch size=%u Q=%u alias=%u seed=%u\n", size, q, alias, seed);
                return 1;
            }
        }
    }
    puts("PMULL_ALIAS_OK");
}

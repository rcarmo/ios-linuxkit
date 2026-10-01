// Native integer oracle for scalar AdvSIMD saturation, including full Vd/FPSR.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } vec;
typedef void (*operation)(const vec *, const vec *, vec *, uint64_t *, uint64_t *, uint64_t);
#define WIDTH(NAME, OP, W) \
    VARIANT(NAME, OP, W, distinct, "2", "0", "1", "q2") \
    VARIANT(NAME, OP, W, left, "0", "0", "1", "q0") \
    VARIANT(NAME, OP, W, right, "1", "0", "1", "q1") \
    VARIANT(NAME, OP, W, same, "2", "0", "0", "q2") \
    VARIANT(NAME, OP, W, all, "0", "0", "0", "q0")
#define VARIANT(NAME, OP, W, SUFFIX, RD, RN, RM, QD) \
static void NAME##_##W##_##SUFFIX(const vec *a, const vec *b, vec *out, \
        uint64_t *fpsr, uint64_t *nzcv, uint64_t initial) { \
    __asm__ volatile( \
        "ldr q0, [%[a]]\n ldr q1, [%[b]]\n movi v2.16b, #255\n" \
        "msr fpsr, %[initial]\n msr nzcv, %[flags]\n" \
        OP " " #W RD ", " #W RN ", " #W RM "\n" \
        "str " QD ", [%[out]]\n mrs %[fpsr], fpsr\n mrs %[nzcv], nzcv\n" \
        : [fpsr] "=r" (*fpsr), [nzcv] "=r" (*nzcv) \
        : [a] "r" (a), [b] "r" (b), [out] "r" (out), \
          [initial] "r" (initial), [flags] "r" (UINT64_C(0xa0000000)) \
        : "v0", "v1", "v2", "memory", "cc"); \
}
#define FAMILY(NAME, OP) WIDTH(NAME, OP, b) WIDTH(NAME, OP, h) WIDTH(NAME, OP, s) WIDTH(NAME, OP, d)
FAMILY(sqadd, "sqadd") FAMILY(uqadd, "uqadd") FAMILY(sqsub, "sqsub") FAMILY(uqsub, "uqsub")
#define ROW(OP, W) {OP##_##W##_distinct, OP##_##W##_left, OP##_##W##_right, OP##_##W##_same, OP##_##W##_all}
#define TABLE(OP) {ROW(OP,b), ROW(OP,h), ROW(OP,s), ROW(OP,d)}
static const operation ops[4][4][5] = {TABLE(sqadd), TABLE(uqadd), TABLE(sqsub), TABLE(uqsub)};
static void gzip_site(const vec *a, const vec *b, vec *out, uint64_t *fpsr,
        uint64_t *nzcv, uint64_t initial) {
    __asm__ volatile("ldr q30, [%[a]]\n ldr q31, [%[b]]\n"
        "msr fpsr, %[initial]\n msr nzcv, %[flags]\n"
        ".inst 0x7e7f2fde\n str q30, [%[out]]\n mrs %[fpsr], fpsr\n mrs %[nzcv], nzcv\n"
        : [fpsr] "=r" (*fpsr), [nzcv] "=r" (*nzcv)
        : [a] "r" (a), [b] "r" (b), [out] "r" (out), [initial] "r" (initial),
          [flags] "r" (UINT64_C(0xa0000000)) : "v30", "v31", "memory", "cc");
}
static unsigned cases;
static int check(operation fn, unsigned op, unsigned bits, uint64_t a, uint64_t b, uint64_t initial) {
    unsigned __int128 limit = (unsigned __int128)1 << bits;
    uint64_t mask = (uint64_t)(limit - 1);
    a &= mask; b &= mask;
    __int128 x = a, y = b, low = 0, high = limit - 1;
    if (!(op & 1)) {
        unsigned __int128 sign = limit >> 1;
        if ((unsigned __int128)x & sign) x -= (__int128)limit;
        if ((unsigned __int128)y & sign) y -= (__int128)limit;
        low = -(__int128)sign; high = sign - 1;
    }
    __int128 result = op < 2 ? x + y : x - y;
    int saturated = result < low || result > high;
    if (result < low) result = low;
    if (result > high) result = high;
    vec va = {a | (~mask), UINT64_MAX}, vb = {b | (~mask), UINT64_MAX}, actual;
    uint64_t fpsr, nzcv;
    fn(&va, &vb, &actual, &fpsr, &nzcv, initial);
    uint64_t expected = (uint64_t)result & mask;
    uint64_t flags = initial | (saturated ? UINT64_C(1) << 27 : 0);
    cases++;
    if (actual.lo != expected || actual.hi || fpsr != flags || nzcv != UINT64_C(0xa0000000)) {
        printf("FAIL case=%u op=%u bits=%u a=%016" PRIx64 " b=%016" PRIx64
            " got=%016" PRIx64 "/%016" PRIx64 " expected=%016" PRIx64
            " fpsr=%08" PRIx64 "/%08" PRIx64 " nzcv=%08" PRIx64 "\n",
            cases, op, bits, a, b, actual.lo, actual.hi, expected, fpsr, flags, nzcv);
        return 1;
    }
    return 0;
}
int main(void) {
    unsigned failures = 0;
    for (unsigned op = 0; op < 4; op++) for (unsigned size = 0; size < 4; size++) {
        unsigned bits = 8u << size;
        uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
        uint64_t sign = UINT64_C(1) << (bits - 1);
        uint64_t values[] = {0, 1, 2, sign - 2, sign - 1, sign, sign + 1, mask - 1, mask};
        for (unsigned alias = 0; alias < 5; alias++)
            for (unsigned i = 0; i < 9; i++) for (unsigned j = 0; j < 9; j++)
                for (unsigned sticky = 0; sticky < 3; sticky++) {
                    uint64_t initial[] = {0, UINT64_C(1) << 27, (UINT64_C(1) << 27) | 0x11};
                    failures += check(ops[op][size][alias], op, bits, values[i],
                        alias >= 3 ? values[i] : values[j], initial[sticky]);
                }
    }
    for (unsigned i = 0; i < 65536; i += 257)
        failures += check(gzip_site, 3, 16, i, 0x8000, 0);
    printf("scalar-saturation-ok cases=%u failures=%u\n", cases, failures);
    return failures != 0;
}

/* Compile the real detector against platform API mocks. No crypto instruction
 * runs here; simulate both feature choices on either physical host. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>

static const char *override;
static unsigned long caps;
static int calls;
#if defined(TEST_DARWIN)
static int primary_error, legacy_error, primary_value, legacy_value, short_size;
#undef __linux__
#define __APPLE__ 1
#endif
#if defined(TEST_NONARM)
#undef __aarch64__
#endif
char *sha512_mock_getenv(const char *name) {
    assert(strcmp(name, "ISH_HOST_SHA512") == 0);
    return (char *)override;
}
unsigned long sha512_mock_getauxval(unsigned long type) {
    assert(type == AT_HWCAP); calls++; return caps;
}
#if defined(TEST_DARWIN)
int sha512_mock_sysctlbyname(const char *name, void *out, size_t *size, const void *in, size_t n) {
    assert(in == NULL && n == 0 && *size == sizeof(int));
    calls++;
    if (strcmp(name, "hw.optional.arm.FEAT_SHA512") == 0) {
        *(int *)out = primary_value;
        if (short_size) *size = 1;
        return primary_error;
    }
    assert(strcmp(name, "hw.optional.armv8_2_sha512") == 0);
    *(int *)out = legacy_value;
    return legacy_error;
}
#define sysctlbyname sha512_mock_sysctlbyname
#endif
#define getenv sha512_mock_getenv
#define getauxval sha512_mock_getauxval
#include "../../../asbestos/guest-arm64/crypto_helpers.c"

static void check(int expected, int expected_calls) {
    calls = 0;
    ish_host_sha512 = 1; // failures must reset, not retain a stale positive
    detect_host_sha512();
    assert(ish_host_sha512 == expected);
    assert(calls == expected_calls);
}
int main(void) {
    const char *values[] = {NULL, "1", "", "garbage", "00"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        override = values[i];
#if defined(TEST_NONARM)
        caps = HWCAP_SHA512; check(0, 0);
#elif defined(TEST_DARWIN)
        primary_error = 0; primary_value = 0; legacy_value = 1; check(0, 1);
        primary_value = 1; check(1, 1);
        primary_error = -1; legacy_error = 0; legacy_value = 1; check(1, 2);
        legacy_value = 0; check(0, 2);
        legacy_error = -1; legacy_value = 1; check(0, 2);
        primary_error = 0; primary_value = 1; short_size = 1; check(0, 1);
        short_size = 0;
#else
        caps = HWCAP_SHA2; check(0, 1);
        caps = HWCAP_SHA512 | HWCAP_SHA2; check(1, 1);
        caps = 0; check(0, 1);
#endif
    }
    override = "0"; caps = HWCAP_SHA512;
    check(0, 0);
    puts("sha512-fallback-detector-ok");
    return 0;
}

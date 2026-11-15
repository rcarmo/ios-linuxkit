/* Test-only Darwin declaration, for compiling the real detector on Linux. */
#ifndef SHA512_FALLBACK_TEST_SYSCTL_H
#define SHA512_FALLBACK_TEST_SYSCTL_H
#include <stddef.h>
int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
#endif

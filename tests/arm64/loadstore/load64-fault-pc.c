#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "requires native AArch64"
#endif

/* Keep the instruction before LDR observable. Retrying the block rather than
 * the LDR increments x5 twice; skipping it or reporting an earlier PC fails in
 * the signal handler. CBZ/CBNZ must remain immediately adjacent to the load so
 * the translator exercises its load/branch fusion as well as the plain path.
 */
#define LOAD_CASE(name, branch) \
    __asm__(".text\n.p2align 4\n.global " #name "\n" \
            ".type " #name ", %function\n" #name ":\n" \
            "mov x5, #0\nmov x6, #0x55\nmov x3, #0\n" \
            "add x5, x5, #1\n.global " #name "_pc\n" #name "_pc:\n" \
            "ldr x6, [x0, #8]\n" branch \
            "b 2f\n1: mov x3, #1\n2:\n" \
            "str x6, [x1]\nstr x5, [x1, #8]\nstr x3, [x1, #16]\nret\n" \
            ".size " #name ", .-" #name "\n"); \
    extern void name(const void *, uint64_t *); \
    extern const char name##_pc[]

LOAD_CASE(load_plain, "");
LOAD_CASE(load_cbz, "cbz x6, 1f\n");
LOAD_CASE(load_cbnz, "cbnz x6, 1f\n");

static void *protected_page;
static size_t page_size;
static uintptr_t expected_pc;
static volatile sig_atomic_t faults;
static unsigned char saved_pages[8192];

static void handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    ucontext_t *uc = context;
    if (sig != SIGSEGV || uc->uc_mcontext.pc != expected_pc ||
        uc->uc_mcontext.regs[5] != 1 || uc->uc_mcontext.regs[6] != 0x55)
        _exit(40);
    if (++faults != 1)
        _exit(41);
    void *p = mmap(protected_page, page_size * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != protected_page)
        _exit(42);
    memcpy(p, saved_pages, page_size * 2);
}

int main(void) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != 4096)
        return 10;
    // Isolated fixed region: avoid both grow-down stack recovery and the
    // existing near-neighbor read-fault auto-mapping workaround. Removing
    // both pages below gives the handler a real SIGSEGV even for split loads.
    unsigned char *mem = mmap((void *)0x40000000, page_size * 2, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mem == MAP_FAILED)
        return 11;
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL))
        return 12;
    alarm(30);
    void (*loads[])(const void *, uint64_t *) = {load_plain, load_cbz, load_cbnz};
    const char *pcs[] = {load_plain_pc, load_cbz_pc, load_cbnz_pc};
    const size_t offsets[] = {64, 67, 4092};
    const uint64_t values[] = {0, UINT64_C(0xfedcba9876543210)};
    unsigned cases = 0;
    for (unsigned fn = 0; fn < 3; fn++) {
        for (unsigned off = 0; off < 3; off++) {
            for (unsigned val = 0; val < 2; val++) {
                unsigned char *address = mem + offsets[off];
                memcpy(address, &values[val], 8);
                uint64_t expected_branch = fn == 1 ? val == 0 : fn == 2 ? val != 0 : 0;
                uint64_t out[3] = {0};
                // Warm code and translations, then remove both backing pages.
                // The second invocation must fault and retry precisely LDR.
                // The handler restores both pages. For split loads this tests
                // first-page fault recovery, not a second-page-only fault.
                loads[fn](address - 8, out);
                if (out[0] != values[val] || out[1] != 1 || out[2] != expected_branch)
                    return 20;
                protected_page = mem;
                expected_pc = (uintptr_t)pcs[fn];
                faults = 0;
                memcpy(saved_pages, protected_page, page_size * 2);
                if (munmap(protected_page, page_size * 2))
                    return 21;
                loads[fn](address - 8, out);
                if (faults != 1 || out[0] != values[val] || out[1] != 1 || out[2] != expected_branch) {
                    fprintf(stderr, "fn=%u off=%zu val=%u faults=%d out=%llx,%llu,%llu expected=%llx,1,%llu\n",
                            fn, offsets[off], val, faults, (unsigned long long)out[0],
                            (unsigned long long)out[1], (unsigned long long)out[2],
                            (unsigned long long)values[val], (unsigned long long)expected_branch);
                    return 22;
                }
                cases++;
            }
        }
    }
    if (munmap(mem, page_size * 2))
        return 23;
    printf("load64-fault-pc-ok cases=%u\n", cases);
    return 0;
}

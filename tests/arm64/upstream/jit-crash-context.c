// Exercise the actual CLI signal handler, not a duplicated recovery model.
// Discard the renamed CLI entry point at link time (see run.sh).
#define main ish_cli_main_unused
#include "../../../main.c"
#undef main
#include <assert.h>

__thread volatile sig_atomic_t in_jit;
__thread volatile uint64_t jit_saved_pc;
__thread volatile uint64_t jit_last_host_fault, jit_last_x7, jit_last_x10;
__thread volatile int jit_crash_count;
void jit_crash_trampoline(void) {}

static void one_case(int sig, bool write_fault, bool precise) {
    struct fiber_frame frame = {0};
    const addr_t saved = 0x10001234, fallback = 0x20005678;
    frame.jit_saved_pc = precise ? saved : 0;
    frame.jit_exit_sp = 0x12345000;
    frame.cpu.pc = 0xdead0000;
    jit_saved_pc = fallback;
    // Poison padding too: a four-byte store to the bool must be detected even
    // if today's layout happens to put only padding after it.
    size_t flag = offsetof(struct fiber_frame, cpu.segfault_was_write);
    memset((char *) &frame + flag, 0xa5, 4);
    frame.cpu.trapno = 0xfeedface;
    struct fiber_frame expected;
    memcpy(&expected, &frame, sizeof(expected)); // Preserve poisoned padding too.
    expected.cpu.segfault_addr = 0x4567;
    expected.cpu.segfault_was_write = write_fault;
    expected.cpu.pc = precise ? saved : fallback;

    ucontext_t uc = {0};
#ifdef __APPLE__
    struct __darwin_mcontext64 mc = {0};
    uc.uc_mcontext = &mc;
    mc.__ss.__x[1] = (uintptr_t) &frame.cpu;
    mc.__ss.__x[7] = 0x10004567;
    mc.__ss.__x[10] = 0x10000000;
    mc.__es.__esr = write_fault ? 0x40 : 0;
#else
    uc.uc_mcontext.regs[1] = (uintptr_t) &frame.cpu;
    uc.uc_mcontext.regs[7] = 0x10004567;
    uc.uc_mcontext.regs[10] = 0x10000000;
    struct esr_context esr = {
        .head = {.magic = ESR_MAGIC, .size = sizeof(esr)},
        .esr = write_fault ? 0x40 : 0,
    };
    memcpy(uc.uc_mcontext.__reserved, &esr, sizeof(esr));
#endif
    siginfo_t info = {.si_addr = (void *) 0x10004567};
    sigset_t before, after;
    assert(sigprocmask(SIG_SETMASK, NULL, &before) == 0);
    in_jit = 1;
    crash_handler(sig, &info, &uc);
    in_jit = 0;
    // The handler normally unblocks its signal before returning to a
    // trampoline. Preserve the harness's original mask after direct calls.
    assert(sigprocmask(SIG_SETMASK, &before, &after) == 0);
    assert(host_ctx_aarch64_pc(&uc) == (uintptr_t) jit_crash_trampoline);
    assert(host_ctx_aarch64_sp(&uc) == frame.jit_exit_sp);
    assert(jit_last_host_fault == (uintptr_t) info.si_addr);
    assert(jit_last_x7 == 0x10004567 && jit_last_x10 == 0x10000000);
    if (memcmp(&frame, &expected, sizeof(frame))) {
        for (size_t i = 0; i < sizeof(frame); i++)
            if (((unsigned char *) &frame)[i] != ((unsigned char *) &expected)[i])
                fprintf(stderr, "crash-context: unexpected byte change at %zu (flag=%zu)\n", i, flag);
        abort();
    }
}

int main(void) {
    for (unsigned sig = 0; sig < 2; sig++)
        for (unsigned write_fault = 0; write_fault < 2; write_fault++)
            for (unsigned precise = 0; precise < 2; precise++)
                one_case(sig ? SIGBUS : SIGSEGV, write_fault, precise);
    assert(jit_crash_count == 8);
    puts("jit-crash-context-ok cases=8 exact-byte-footprint precise/fallback-PC SP/trampoline");
    return 0;
}

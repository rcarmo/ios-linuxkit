// Exercise the actual CLI signal handler, not a duplicated recovery model.
// Discard the renamed CLI entry point at link time (see run.sh).
#define main ish_cli_main_unused
#include "../../../main.c"
#undef main
#include <assert.h>

// TLS/trampoline definitions come from the candidate archives, matching its
// optional native frame ABI; do not duplicate them in this host fixture.

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
    // An uninstalled app adapter must never replay a gadget fault or mutate
    // frame/context/signal mask. CLI fallback remains separately admitted.
    struct fiber_frame untouched_frame; memcpy(&untouched_frame,&frame,sizeof(frame));
    ucontext_t untouched=uc;
#ifdef __APPLE__
    struct __darwin_mcontext64 untouched_mc=mc;
#endif
    assert(ish_app_native_fault_recover(sig,&info,&uc)==ISH_FAULT_UNHANDLED);
    assert(!memcmp(&uc,&untouched,sizeof(uc)));
#ifdef __APPLE__
    assert(!memcmp(&mc,&untouched_mc,sizeof(mc)));
#endif
    assert(!memcmp(&frame,&untouched_frame,sizeof(frame)));
    sigset_t app_mask; assert(sigprocmask(SIG_SETMASK,NULL,&app_mask)==0);
    for(int signo=1;signo<NSIG;signo++) assert(sigismember(&app_mask,signo)==sigismember(&before,signo));
#ifdef ISH_JIT
    // Exact checkpoint context: malformed x1 must fail-stop without mutation.
    frame.native_fault_host_pc=0x123450;
    frame.native_fault_guest_pc=0x345600;
    frame.native_fault_addr=0x6789;
    frame.native_fault_write=1;
    jit_active_frame=&frame;
    host_ctx_aarch64_set_pc(&uc,frame.native_fault_host_pc);
#ifdef __APPLE__
    mc.__ss.__x[1]=0;
#else
    uc.uc_mcontext.regs[1]=0;
#endif
    assert(ish_app_native_fault_recover(sig,&info,&uc)==ISH_FAULT_FATAL);
    assert(frame.native_fault_host_pc==0x123450 && frame.cpu.pc==untouched_frame.cpu.pc);
    memcpy(&frame,&untouched_frame,sizeof(frame)); jit_active_frame=NULL;
    uc=untouched;
#ifdef __APPLE__
    mc=untouched_mc;
#endif
#endif
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

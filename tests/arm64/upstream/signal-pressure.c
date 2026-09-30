// Actual receive_signals/user_write path; wrap only final group termination.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/memory_policy.h"
#include "kernel/signal.h"
#include "fs/real.h"
#include "util/timer.h"
static jmp_buf terminated;
static void feed(bool critical) {
    struct ish_memory_capture capture = ish_memory_policy_capture();
    struct timespec clock = timespec_now(CLOCK_MONOTONIC);
    assert(ish_memory_policy_update((struct ish_memory_sample) {
        100 * real_page_size, 100 * real_page_size,
        (uint64_t) clock.tv_sec * 1000 + clock.tv_nsec / 1000000, critical, capture}));
}
noreturn void __wrap_do_exit_group(int status) {
    assert(status == SIGSEGV_);
    // Termination must be entered without sighand->lock held.
    assert(trylock(&current->sighand->lock) == 0);
    unlock(&current->sighand->lock);
    struct ish_memory_budget budget = ish_memory_policy_budget();
    assert(budget.pending_bytes == 0);
    longjmp(terminated, 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    bool refusal = !strcmp(argv[1], "refuse");
    assert(refusal || !strcmp(argv[1], "recover"));
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    assert(ish_memory_policy_enable(true));
    feed(false);
    assert(pt_map_nothing(current->mem, STACK_INIT_PAGE, 1,
        P_READ | P_WRITE | P_GROWSDOWN) == 0);
    // A signal frame crosses into the next unmapped growsdown page.
    current->cpu.sp = (STACK_INIT_PAGE << PAGE_BITS) + 16;
    current->sighand->action[SIGUSR1_] = (struct sigaction_) {.handler = 0x1234};
    // Handling SIGSEGV must not retry frame delivery indefinitely either.
    current->sighand->action[SIGSEGV_] = (struct sigaction_) {.handler = 0x5678};
    feed(true);
    if (!refusal) feed(false);
    deliver_signal(current, SIGUSR1_, SIGINFO_NIL);
    if (setjmp(terminated) == 0) {
        receive_signals();
        assert(!refusal);
        assert(current->cpu.pc == 0x1234);
        assert(mem_pt(current->mem, STACK_INIT_PAGE - 1) != NULL);
        assert(atomic_load(&anon_page_count) == (long)(STACK_INIT_PAGE - PAGE(current->cpu.sp) + 1));
        puts("signal-pressure-recovery-ok");
    } else {
        assert(refusal);
        assert(mem_pt(current->mem, STACK_INIT_PAGE - 1) == NULL);
        assert(atomic_load(&anon_page_count) == 1);
        puts("signal-pressure-refusal-ok");
    }
    assert(pt_unmap_always(current->mem, PAGE(current->cpu.sp),
        STACK_INIT_PAGE - PAGE(current->cpu.sp) + 1) == 0);
    assert(atomic_load(&anon_page_count) == 0);
    assert(ish_memory_policy_enable(false));
    return 0;
}

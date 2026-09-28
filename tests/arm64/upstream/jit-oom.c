// Link actual JIT/kernel archives with --wrap=calloc/--wrap=malloc.
// OOM must return a distinct interrupt and leave both JIT locks available.
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "emu/cpu.h"
#include "asbestos/frame.h"
#include "kernel/mm.h"
#include "kernel/memory.h"
#include "fs/real.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "asbestos/asbestos.h"
#include "asbestos/gen.h"

void *__real_calloc(size_t, size_t);
void *__real_malloc(size_t);
static bool fail_frame, fail_block;
// Exercise dispatch without actually terminating the test's synthetic init.
_Noreturn void __wrap_do_exit_group(int status) {
    assert(status == SIGKILL_);
    puts("jit-oom-guest-kill-dispatch-ok");
    exit(0);
}
void *__wrap_calloc(size_t n, size_t size) {
    if (fail_frame && n == 1 && size == sizeof(struct fiber_frame)) {
        fail_frame = false;
        return NULL;
    }
    return __real_calloc(n, size);
}
void *__wrap_malloc(size_t size) {
    if (fail_block && size == sizeof(struct fiber_block) + FIBER_BLOCK_INITIAL_CAPACITY * sizeof(unsigned long)) {
        fail_block = false;
        return NULL;
    }
    return __real_malloc(size);
}
static void assert_unlocked(void) {
    struct asbestos *jit = current->mem->mmu.asbestos;
    assert(write_wrtrylock(&jit->jetsam_lock));
    write_wrunlock(&jit->jetsam_lock);
    assert(pthread_mutex_trylock(&jit->lock.m) == 0);
    pthread_mutex_unlock(&jit->lock.m);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0);
    assert(become_first_process() == 0);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(tlb);
    current->cpu.pc = 0x10000;
    fail_frame = true;
    assert(cpu_run_to_interrupt(&current->cpu, tlb) == INT_OOM);
    assert(!fail_frame);
    assert_unlocked();
    fail_block = true;
    assert(cpu_run_to_interrupt(&current->cpu, tlb) == INT_OOM);
    assert(!fail_block);
    assert_unlocked();
    tlb_free(tlb);
    puts("jit-oom-interrupt-locks-ok: frame and block allocation failures");
    handle_interrupt(INT_OOM);
    abort(); // OOM must never be retried as a mapped guest page fault.
}

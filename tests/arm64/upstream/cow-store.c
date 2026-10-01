// Execute a real pre-indexed pair store while another host reader delays CoW.
// A synthetic INT_GPF from a gadget's CoW upgrade is not a safe retry contract.
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "fs/real.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
static struct task *owner;
static pthread_t holder;
static atomic_bool ready, done;
static bool trigger;
static void *hold_read(void *unused) {
    (void)unused;
    read_wrlock(&owner->mem->lock);
    atomic_store(&ready, true);
    struct timespec delay = {.tv_nsec = 20000000};
    nanosleep(&delay, NULL);
    read_wrunlock(&owner->mem->lock);
    atomic_store(&done, true);
    return NULL;
}
int __real_pthread_rwlock_trywrlock(pthread_rwlock_t *);
int __wrap_pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
    if (trigger && current == owner && lock == &owner->mem->lock.l) {
        trigger = false;
        assert(pthread_create(&holder, NULL, hold_read, NULL) == 0);
        while (!atomic_load(&ready)) sched_yield();
    }
    return __real_pthread_rwlock_trywrlock(lock);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    owner = current;
    struct mem child; mem_init(&child);
    const addr_t code = 0x10000, data = 0x20000;
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0, MEM_PAGES) == 0);
    assert(pt_map_nothing(current->mem, PAGE(code), 1, P_READ | P_WRITE | P_EXEC) == 0);
    assert(pt_map_nothing(current->mem, PAGE(data), 1, P_READ | P_WRITE) == 0);
    const uint32_t instructions[] = {0xa9bf0820, 0xd4200000}; // stp x0,x2,[x1,#-16]!; brk
    memcpy(mem_pt(current->mem, PAGE(code))->data->data, instructions, sizeof(instructions));
    assert(pt_copy_on_write(current->mem, &child, PAGE(data), 1) == 0);
    write_wrunlock(&current->mem->lock);
    current->cpu.pc = code;
    current->cpu.regs[0] = 0x123456789abcdef0ULL;
    current->cpu.regs[1] = data + 16;
    current->cpu.regs[2] = 0xfedcba9876543210ULL;
    struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
    trigger = true;
    read_wrlock(&current->mem->lock);
    tlb_refresh(tlb, &current->mem->mmu);
    int interrupt = cpu_run_to_interrupt(&current->cpu, tlb);
    read_wrunlock(&current->mem->lock);
    assert(!trigger);
    assert(pthread_join(holder, NULL) == 0 && atomic_load(&done));
    if (interrupt != INT_BREAKPOINT)
        fprintf(stderr, "cow-store: unexpected interrupt=%d pc=%#llx base=%#llx\n",
                interrupt, (unsigned long long)current->cpu.pc,
                (unsigned long long)current->cpu.regs[1]);
    assert(interrupt == INT_BREAKPOINT);
    assert(current->cpu.pc == code + 4 && current->cpu.regs[1] == data);
    uint64_t values[2];
    assert(user_read(data, values, sizeof(values)) == 0);
    assert(values[0] == current->cpu.regs[0] && values[1] == current->cpu.regs[2]);
    const uint64_t *old = mem_pt(&child, PAGE(data))->data->data;
    assert(old[0] == 0 && old[1] == 0);
    tlb_free(tlb); mem_destroy(&child);
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0, MEM_PAGES) == 0);
    write_wrunlock(&current->mem->lock);
    assert(atomic_load(&anon_page_count) == 0);
    puts("cow-store-contention-ok: gadget STP writeback/data/CoW isolation");
}

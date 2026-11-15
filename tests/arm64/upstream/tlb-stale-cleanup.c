// Exercise the actual static task cleanup, including orphan-mm handoff.
#include "kernel/task.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct task self={0}; current=&self;
    self.mm=mm_new(); assert(self.mm); self.mem=&self.mm->mem;
    struct mm *mm=self.mm; mm_retain(mm);
    struct tlb *tlb=calloc(1,sizeof(*tlb)); assert(tlb);
    tlb_attach(tlb,&mm->mem.mmu); tlb_refresh(tlb,&mm->mem.mmu);
    self.mm_release_deferred=true;
    task_run_tlb_cleanup(tlb);
    assert(!self.mm && !self.mem && !self.mm_release_deferred);
    assert(atomic_load(&mm->refcount)==1 && list_empty(&mm->mem.mmu.tlbs));
    mm_release(mm);
    // Ordinary cleanup detaches but must not consume the task's mm reference.
    self.mm=mm_new(); assert(self.mm); self.mem=&self.mm->mem; mm=self.mm;
    tlb=calloc(1,sizeof(*tlb)); assert(tlb);
    tlb_attach(tlb,&mm->mem.mmu); tlb_refresh(tlb,&mm->mem.mmu);
    task_run_tlb_cleanup(tlb);
    assert(self.mm==mm && atomic_load(&mm->refcount)==1);
    assert(list_empty(&mm->mem.mmu.tlbs)); mm_release(mm);
    current=NULL;
    puts("tlb-stale-cleanup-ok: normal detach and orphan deferred-mm handoff");
}

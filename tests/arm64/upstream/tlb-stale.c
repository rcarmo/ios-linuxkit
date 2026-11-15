// Actual kernel lifecycle plus synthetic translation races and unattached C hits.
#include <assert.h>
#include <stdio.h>
#include "kernel/memory.h"
#include "emu/tlb.h"
#include "asbestos/frame.h"
static char backing[2][PAGE_SIZE];
static unsigned which, translate_count, race_budget;
static void changed(struct mmu *mmu) {
    __atomic_add_fetch(&mmu->changes, 1, __ATOMIC_SEQ_CST);
    mmu_tlbs_stale(mmu);
}
static void *translate(struct mmu *mmu, addr_t addr, int type) {
    (void)addr; (void)type; translate_count++;
    if (race_budget) { race_budget--; which ^= 1; changed(mmu); }
    return backing[which];
}
static void *nofault(struct mmu *mmu, addr_t addr) {
    (void)mmu; (void)addr; return backing[which];
}
static void *mark_thread(void *arg) {
    struct mmu *mmu = arg;
    for (unsigned i = 0; i < 20000; i++) changed(mmu);
    return NULL;
}
int main(void) {
    struct mmu_ops ops = {.translate=translate, .translate_write_nofault=nofault};
    struct mmu mmu = {.ops=&ops};
    lock_init(&mmu.tlbs_lock); list_init(&mmu.tlbs);
    struct tlb *a=calloc(1,sizeof(*a)), *b=calloc(1,sizeof(*b)); assert(a && b);
    tlb_attach(a,&mmu); tlb_refresh(a,&mmu);
    tlb_attach(b,&mmu); tlb_refresh(b,&mmu);
    assert(list_size(&mmu.tlbs)==2);
    assert(__tlb_read_ptr(a,0x4000)==backing[0]);
    assert(__tlb_write_ptr(b,0x4000)==backing[0]);
    which=1; changed(&mmu);
    assert(a->stale==TLB_STALE && b->stale==TLB_STALE);
    assert(__tlb_read_ptr(a,0x4000)==backing[1] && a->stale==0);
    assert(__tlb_write_ptr(b,0x4000)==backing[1] && b->stale==0);
    tlb_detach(a); tlb_detach(a);
    which=0; changed(&mmu);
    assert(a->stale==0 && b->stale==TLB_STALE);
    // C lookups must still reject stale generations even while unattached.
    assert(__tlb_read_ptr(a,0x4000)==backing[0]);
    which=1; changed(&mmu);
    assert(__tlb_write_ptr(a,0x4000)==backing[1]);
    __atomic_store_n(&mmu.changes, (1ull<<32)+7, __ATOMIC_SEQ_CST);
    mmu_tlbs_stale(&mmu);
    assert(__tlb_read_ptr(b,0x4000)==backing[1]);
    assert(b->mem_changes==mmu.changes);
    // A CoW-style translate bumps the generation and retries successfully.
    race_budget=1;
    unsigned before=translate_count;
    assert(tlb_handle_miss(b,0x5000,MEM_WRITE)==backing[0]);
    assert(translate_count==before+2 && b->stale==0);
    // At the cap, never leave an older page valid at the new generation.
    race_budget=20; before=translate_count;
    assert(tlb_handle_miss(b,0x6000,MEM_READ)!=NULL);
    assert(translate_count==before+9 && b->stale==TLB_STALE);
    assert(b->entries[TLB_INDEX(0x4000)].page==TLB_PAGE_EMPTY);
    assert(b->entries[TLB_INDEX(0x6000)].page==TLB_PAGE_EMPTY);
    race_budget=0;
    pthread_t marker; assert(!pthread_create(&marker,NULL,mark_thread,&mmu));
    for (unsigned i=0;i<2000;i++) {
        tlb_attach(a,&mmu); tlb_refresh(a,&mmu);
        tlb_handle_miss(a,0x4000,MEM_READ); tlb_detach(a);
    }
    assert(!pthread_join(marker,NULL));
    tlb_free(b); assert(list_empty(&mmu.tlbs)); tlb_free(a);
    // Refresh must retain local frame/cache invalidation on mapping/exec changes.
    struct mmu other = {.ops=&ops};
    lock_init(&other.tlbs_lock); list_init(&other.tlbs);
    a=calloc(1,sizeof(*a)); assert(a); tlb_refresh(a,&mmu);
    a->frame=calloc(1,sizeof(*a->frame)); assert(a->frame);
    struct fiber_frame *saved=a->frame;
    a->frame->last_block=(void *)1; a->frame->ret_cache[0]=1;
    a->block_cache[0]=(void *)1; a->block_cache_gen=17;
    changed(&mmu); tlb_refresh(a,&mmu);
    assert(a->frame==saved && !a->frame->last_block);
    assert(!a->frame->ret_cache[0] && !a->block_cache[0] && !a->block_cache_gen);
    tlb_attach(a,&mmu); tlb_detach(a); tlb_attach(a,&other); tlb_refresh(a,&other);
    assert(!a->frame && list_empty(&mmu.tlbs) && list_size(&other.tlbs)==1);
    tlb_free(a); assert(list_empty(&other.tlbs));
    assert(!pthread_mutex_destroy(&other.tlbs_lock.m));
    assert(!pthread_mutex_destroy(&mmu.tlbs_lock.m));
    // Exercise real init, marking, free-detach and destructor repeatedly.
    for (unsigned i=0;i<20;i++) {
        struct mem mem; mem_init(&mem);
        a=calloc(1,sizeof(*a)); assert(a);
        read_wrlock(&mem.lock); tlb_attach(a,&mem.mmu); tlb_refresh(a,&mem.mmu);
        mem_changed_pub(&mem); assert(a->stale==TLB_STALE);
        tlb_free(a); read_wrunlock(&mem.lock); mem_destroy(&mem);
    }
    printf("tlb-stale-ok: lifecycle, unattached read/write, 64-bit generations, CoW retry, cap, concurrent mark/detach; stale=%zu entries=%zu delta=%zd\n",
           offsetof(struct tlb,stale),offsetof(struct tlb,entries),
           (ptrdiff_t)offsetof(struct tlb,stale)-(ptrdiff_t)offsetof(struct tlb,entries));
}

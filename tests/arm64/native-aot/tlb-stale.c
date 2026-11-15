// Execute the actual emitter's inline lookup, including stale and boundary misses.
#include "asbestos/guest-arm64/jit.c"
#include <assert.h>
extern void probe_call(void *, struct cpu_state *, void *);
extern void tlb_stale_gadget_read(void), tlb_stale_gadget_write(void);
static void *lookup(addr_t addr, unsigned bytes, bool write) {
    struct em e={0}; mov_imm64(&e,T_A,addr); tlb_lookup(&e,bytes,write,0);
    put(&e,0xF9000029u | ((CPU_pc/8)<<10)); // str x9,[x1,#CPU_pc]
    put(&e,0xD65F03C0u);
    unsigned miss=e.n; mov_imm64(&e,9,0);
    put(&e,0xF9000029u | ((CPU_pc/8)<<10)); put(&e,0xD65F03C0u);
    for(unsigned i=0;i<e.nfix;i++) {
        assert(e.fix[i].kind==1 || e.fix[i].kind==2);
        e.buf[e.fix[i].at]=0x54000000u | (((miss-e.fix[i].at)&0x7ffffu)<<5) |
            (e.fix[i].kind==1?1:8);
    }
    void *dst=region_alloc(e.n*4); assert(dst); install_code(dst,e.buf,e.n); return dst;
}
int main(void) {
    assert(map_dual()); pic_on=true; pin_init(true); make_exit_stub();
    struct tlb *tlb=calloc(1,sizeof(*tlb)); assert(tlb);
    struct fiber_frame frame={0};
    const addr_t va=0xab1234567000ull;
    struct tlb_entry *entry=&tlb->entries[TLB_INDEX(va)];
    entry->page=entry->page_if_writable=va;
    entry->data_minus_addr=0x12340000-va;
    for(unsigned write=0;write<2;write++) {
        void *code=lookup(va+24,8,write);
        tlb->stale=0; probe_call(code,&frame.cpu,tlb->entries);
        assert(frame.cpu.pc==0x12340018);
        tlb->stale=TLB_STALE; probe_call(code,&frame.cpu,tlb->entries);
        assert(frame.cpu.pc==0);
        tlb->stale=0; code=lookup(va+4093,8,write);
        probe_call(code,&frame.cpu,tlb->entries); assert(frame.cpu.pc==0);
        code=lookup(va+4096,8,write);
        probe_call(code,&frame.cpu,tlb->entries); assert(frame.cpu.pc==0);
    }
    for(unsigned write=0;write<2;write++) {
        void *gadget=write?(void *)tlb_stale_gadget_write:(void *)tlb_stale_gadget_read;
        tlb->stale=0; frame.cpu.pc=va+24;
        probe_call(gadget,&frame.cpu,tlb->entries); assert(frame.cpu.pc==0x12340018);
        tlb->stale=TLB_STALE; frame.cpu.pc=va+24;
        probe_call(gadget,&frame.cpu,tlb->entries); assert(frame.cpu.pc==0);
        tlb->stale=0; frame.cpu.pc=va+4093;
        probe_call(gadget,&frame.cpu,tlb->entries); assert(frame.cpu.pc==0);
        frame.cpu.pc=va+4096;
        probe_call(gadget,&frame.cpu,tlb->entries); assert(frame.cpu.pc==0);
    }
    assert(JIT_CODE_VERSION==15);
    printf("tlb-stale-emitter-ok: ABI=%08x codeversion=%d JIT+gadgets read/write/hit/stale/cross-page/wrong-page\n",jit_abi(),JIT_CODE_VERSION);
    free(tlb);
}

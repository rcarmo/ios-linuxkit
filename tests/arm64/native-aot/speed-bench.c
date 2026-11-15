// Development-host microbenchmark of actual emitted code. Build against a
// frozen baseline jit.c and candidate jit.c; not an application/device claim.
#include "asbestos/guest-arm64/jit.c"
#include <assert.h>
#include <time.h>
extern void probe_call(void *, struct cpu_state *, void *);
static uint64_t ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(int argc, char **argv) {
    assert(argc==2); bool memory=!strcmp(argv[1],"memory");
    assert(memory || !strcmp(argv[1],"overwrite"));
    pin_init(true);pic_on=false;simd_on=true;
    struct em e={0};memset(e.host,-1,sizeof(e.host));memset(e.owner,-1,sizeof(e.owner));memcpy(e.pin,pin_of,sizeof(e.pin));
    load_pinned(&e);bool mem;
    for(unsigned i=0;i<32;i++) {
        unsigned rd=9+i%4;
        uint32_t op=memory?0xf9400089u:0xd2800000u|(42u<<5)|rd; // ldr x9,[x4] or movz x9..12,#42
        assert(emit_insn(&e,op,0x1000+i*8,i*2,&mem)&&!e.fail);
        assert(emit_insn(&e,0x8b000000u|((memory?9:rd)<<16),0x1004+i*8,i*2+1,&mem)&&!e.fail); // add x0,x0,xN
    }
    for(int i=0;i<n_pinned;i++)put(&e,0xf9000000u|((greg_off(pin_guest[i])/8)<<10)|(1u<<5)|pin_host[i]);
    put(&e,0xd65f03c0);unsigned words=e.n;unsigned trap=e.n;put(&e,0);
    for(unsigned i=0;i<e.nfix;i++){assert(e.fix[i].kind==1||e.fix[i].kind==2);e.buf[e.fix[i].at]=0x54000000u|((trap-e.fix[i].at)<<5)|(e.fix[i].kind==1?1u:8u);}
    void *code=mmap(NULL,65536,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);
    memcpy(code,e.buf,e.n*4);__builtin___clear_cache(code,(char *)code+e.n*4);assert(!mprotect(code,65536,5));
    uint64_t value=42;struct mmu mmu={0};struct tlb *tlb=calloc(1,sizeof(*tlb));assert(tlb);tlb->mmu=&mmu;
    unsigned ix=4;tlb->entries[ix].page=tlb->entries[ix].page_if_writable=0x4000;tlb->entries[ix].data_minus_addr=(uintptr_t)&value-0x4000;
    struct fiber_frame frame={0};frame.cpu.regs[4]=0x4000;
    const unsigned warmup=10000,runs=4000000;
    for(unsigned i=0;i<warmup;i++)probe_call(code,&frame.cpu,tlb->entries);
    uint64_t start=ns();for(unsigned i=0;i<runs;i++)probe_call(code,&frame.cpu,tlb->entries);uint64_t elapsed=ns()-start;
    assert(frame.cpu.regs[0]==(uint64_t)(warmup+runs)*32*42);
    printf("{\"workload\":\"%s\",\"words\":%u,\"iterations\":%u,\"ns\":%lu,\"checksum\":%lu}\n",argv[1],words,runs,elapsed,frame.cpu.regs[0]);
    munmap(code,65536);free(tlb);
}

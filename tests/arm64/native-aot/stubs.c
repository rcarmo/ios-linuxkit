// Real AArch64 shared paths, including high context indices and wrap/refusal.
#include "asbestos/guest-arm64/jit.c"
#include <assert.h>
extern void probe_call(void *, struct cpu_state *, void *);
__asm__(".text\n.global fiber_ret\nfiber_ret:\nret\n.global fiber_ret_chain\nfiber_ret_chain:\nret\n");
static void *install(struct em *e) {
    assert(!e->fail);
    uint8_t *dst=region_alloc(e->n*4); assert(dst);
    for(unsigned i=0;i<e->nfix;i++) {
        assert(e->fix[i].kind==4);
        int64_t d=e->fix[i].abs-((uintptr_t)dst+4*e->fix[i].at);
        assert(fits(d>>2,26));e->buf[e->fix[i].at]=ENC_B(d);
    }
    install_code(dst,e->buf,e->n);return dst;
}
int main(void) {
    assert(map_dual());pic_on=true;pin_init(true);make_exit_stub();make_stubs();
    for(unsigned i=0;i<NSTUBS;i++)assert(stubs[i]);
    for(unsigned i=1;i<NSTUBS;i++)assert(stub_rec[i] && stub_rec[i]->nrel);
    const unsigned idx=0x10001;
    char *storage=calloc(1,CTX_FAR*(idx+1)+CTX_BLK+CTX_SLOT*(idx+1));assert(storage);
    struct jit_ctx *ctx=(void *)(storage+CTX_FAR*(idx+1));
    struct fiber_block *from=calloc(1,sizeof(*from)+32),*to=calloc(1,sizeof(*to)+32);
    assert(from&&to);from->addr=0x1000;to->addr=0x2000;to->jit_ctx=ctx;
    ctx->slot[idx].blk=from;from->jump_ip[0]=&from->code[1];
    to->code[0]=(uintptr_t)jit_fiber_ret;
    struct em *e=calloc(1,sizeof(*e));assert(e);e->idx=idx;e->link_at[0]=e->link_at[1]=-1;
    load_pinned(e);mov_imm64(e,29,(uintptr_t)ctx);
    unsigned start=e->n;emit_chain_ex(e,from,from->jump_ip[0],0,false);
    assert(e->n-start<32 && e->link_at[0]>=0);
    void *code=install(e);
    struct fiber_frame frame={0};
    for(unsigned mode=0;mode<4;mode++) {
        memset(&frame,0,sizeof(frame));frame.cpu.regs[0]=0x123456789abcdef0ull;
        frame.cpu.cycle=mode==1||mode==3?1023:7;
        from->code[1]=mode<2?(uintptr_t)to->code:((1ull<<63)|0x3000);
        if(mode>=2) {from->code[1]|=1ull<<62;ctx_far(ctx,idx)->alt[0]=(uintptr_t)to->code;}
        probe_call(code,&frame.cpu,NULL);
        assert(frame.cpu.regs[0]==0x123456789abcdef0ull);
        assert(frame.cpu.cycle==(mode==1||mode==3?1024:8));
        assert(frame.cpu.pc==(mode==3?0x3000:0x2000));
        assert(frame.last_block==(mode==3?from:to));
    }
    // A same-context native successor must enter at +4, never at the cold
    // prologue. Deliberately poison the first word to detect that mistake.
    struct em native={0};put(&native,0);mov_imm64(&native,11,to->addr);
    put(&native,0xF9000000u|((CPU_pc/8)<<10)|(1u<<5)|11);
    mov_imm64(&native,8,(uintptr_t)to);
    put(&native,0xF9000000u|((LOCAL_last_block/8)<<10)|(1u<<5)|8);
    emit_exit(&native,(uintptr_t)jit_fiber_ret);
    to->native_entry=(uintptr_t)install(&native);
    memset(&frame,0,sizeof(frame));from->code[1]=(uintptr_t)to->code;
    probe_call(code,&frame.cpu,NULL);
    assert(frame.cpu.pc==to->addr && frame.last_block==to && frame.cpu.cycle==1);
    printf("shared-stubs-ok: ABI=%08x codeversion=%d; generic gadget/native, poke, high-index refused, wrap bypass, pinned/cycle parity\n",jit_abi(),JIT_CODE_VERSION);
    free(e);free(from);free(to);free(storage);return 0;
}

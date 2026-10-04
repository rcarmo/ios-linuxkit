// Actual backend read-only diagnostic, before and after normal no-emitter init.
#include "asbestos/guest-arm64/jit.c"
#include <assert.h>
#include <unistd.h>
static bool inspecting;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_mmap(void *,size_t,int,int,int,off_t);
void *__wrap_malloc(size_t n) { assert(!inspecting); return __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t s) { assert(!inspecting); return __real_calloc(n,s); }
void *__wrap_mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {
    assert(!inspecting); return __real_mmap(p,n,prot,flags,fd,off);
}
int main(int argc,char **argv) {
    assert(argc==2); assert(jit_layout_read(NULL)==-1);
    struct jit_layout before,after;
    inspecting=true;
    assert(jit_layout_read(&before)==0);
    assert(!before.ready && !before.abi && !before.n_pinned && !before.prologue_words);
    assert(!before.emission_compiled && before.pointer_bits==64 && before.little_endian);
    assert(before.cpu_size==sizeof(struct cpu_state) && before.frame_size==sizeof(struct fiber_frame));
    assert(before.fault_host_pc==offsetof(struct fiber_frame,native_fault_host_pc));
    char text[2048],again[2048],tiny[3]={'X','Y','Z'};
    size_t n=jit_layout_describe(text,sizeof(text));
    assert(n>0 && n<sizeof(text)-1 && text[n]==0);
    assert(jit_layout_describe(tiny+1,1)==0 && tiny[0]=='X' && tiny[1]==0 && tiny[2]=='Z');
    assert(jit_layout_describe(NULL,16)==0 && jit_layout_describe(tiny,0)==0);
    assert(jit_layout_read(&after)==0 && !memcmp(&before,&after,sizeof(before)));
    assert(!region && !n_pinned && !prologue_words && !jit_on);
    inspecting=false;
    if(!strcmp(argv[1],"init")) {
        // An ABI-invalid image is rejected by normal init, not relabelled by
        // diagnostics. It still lets normal init select PIC/pinning conventions.
        static struct aot_module rejected={.path="layout-fixture",.abi=1};
        ish_aot_register(&rejected);
    } else if(!strcmp(argv[1],"prepare")) {
        assert(jit_aot_prepare_layout()==0);
        assert(!region && !jit_on && !aot_nregistered && !aot_nimages);
        assert(setenv("ISH_JIT","0",1)==0);
    } else if(!strcmp(argv[1],"off")) assert(setenv("ISH_JIT","0",1)==0);
    else assert(!strcmp(argv[1],"empty"));
    assert(jit_units_begin()==NULL); // no accepted images or executable region
    inspecting=true;
    assert(jit_layout_read(&after)==0);
    assert(after.ready==(!strcmp(argv[1],"init") || !strcmp(argv[1],"prepare")));
    if(after.ready) {
        assert(after.abi==jit_abi() && after.abi==0x3f650e41 && after.code_version==10);
        assert(after.pic && after.n_pinned==(unsigned)n_pinned && after.entry_off==entry_off());
    } else assert(!after.abi && !after.n_pinned);
    struct jit_layout copy;
    assert(jit_layout_read(&copy)==0 && !memcmp(&copy,&after,sizeof(after)));
    size_t len=jit_layout_describe(again,sizeof(again));
    assert(len && len<sizeof(again)-1 && again[len]==0 && !region && !jit_on);
    inspecting=false;
    printf("layout-diagnostic-ok mode=%s readonly/no-init/no-allocation/no-mapping\n%s",argv[1],again);
}

// Exercise the actual emitter, not a copied approximation. Link with section
// GC so unused decoder/gadget paths (and their external symbols) are discarded.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int fail_malloc, fail_realloc;
static void *probe_malloc(size_t n) { return fail_malloc ? NULL : malloc(n); }
static void *probe_realloc(void *p, size_t n) { return fail_realloc ? NULL : realloc(p,n); }
#define malloc probe_malloc
#define realloc probe_realloc
#include "../../../asbestos/guest-arm64/gen.c"
#undef malloc
#undef realloc
int main(void) {
    struct gen_state s;
    fail_malloc=1;
    gen_start(0x10000,&s);
    assert(s.oom && s.block==NULL && s.size==0);
    gen(&s,123); assert(s.size==0);
    fail_malloc=0;
    gen_start(0x10000,&s);
    assert(!s.oom && s.block);
    unsigned capacity=s.capacity;
    for(unsigned i=0;i<capacity;i++)gen(&s,i);
    struct fiber_block *old=s.block;
    fail_realloc=1;
    gen(&s,0xdead);
    assert(s.oom && s.block==old && s.size==capacity && s.capacity==capacity);
    for(unsigned i=0;i<capacity;i++)assert(s.block->code[i]==i);
    gen(&s,0xbeef); assert(s.size==capacity);
    free(s.block);
    fail_realloc=0;
    gen_start(0x10000,&s);
    for(unsigned i=0;i<capacity+1;i++)gen(&s,i);
    assert(!s.oom && s.capacity==capacity*2 && s.size==capacity+1);
    free(s.block);
    puts("gen-oom-actual-emitter-ok");
}

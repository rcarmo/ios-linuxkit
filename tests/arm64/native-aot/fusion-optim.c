#define _GNU_SOURCE
// Production generator, production native gadget bodies, architectural model.
#ifndef FUSION_GEN_SOURCE
#define FUSION_GEN_SOURCE "asbestos/guest-arm64/gen.c"
#endif
#include FUSION_GEN_SOURCE
#include "asbestos/guest-arm64/jit.c"
#include <time.h>
extern void fusion_call(void *, struct cpu_state *);
extern void fusion_stop(void);
volatile bool g_trace_highbits;
bool asbestos_should_trace_guest_pc(addr_t pc) { return false; }
static unsigned misses;
void *tlb_handle_miss(struct tlb *t, addr_t a, int type) { misses++; t->segfault_addr=a; return NULL; }
bool __tlb_read_cross_page(struct tlb *t, addr_t a, char *out, unsigned n) { abort(); }
static uint32_t page[2048] __attribute__((aligned(4096)));
static struct tlb tlb;
static struct mmu mmu;
static void map(addr_t pc) {
 memset(&tlb,0,sizeof(tlb));tlb.mmu=&mmu;
 for(unsigned i=0;i<2;i++) {
  addr_t a=(pc&~4095ull)+i*4096;
  if(i && TLB_INDEX(a)==TLB_INDEX(pc))continue;
  struct tlb_entry *e=&tlb.entries[TLB_INDEX(a)];
  e->page=a;e->data_minus_addr=(uintptr_t)page-(pc&~4095ull);e->gen=mmu.changes;
 }
}
static uint64_t model(addr_t pc,uint32_t a,uint32_t b) {
 uint64_t imm=((a>>29)&3)|(((a>>5)&0x7ffff)<<2);
 int64_t signed_imm=(int64_t)(imm<<43)>>43;
 return (((pc&~4095ull)+(uint64_t)(signed_imm*4096))+((b>>10)&4095))&0xffffffffffffull;
}
static unsigned cases;
static void check(addr_t pc,uint32_t a,uint32_t b) {
 map(pc);page[(pc&4095)/4]=a;page[(pc&4095)/4+1]=b;
 struct gen_state s={0};gen_start(pc,&s);
 struct jit_units units={0};struct jit_step step=jit_step_begin(&s);
 if(gen_step(&s,&tlb)!=1){fprintf(stderr,"failure pc=%lx a=%x b=%x cases=%u\n",pc,a,b,cases);abort();}assert(!s.oom && s.ip==pc+8);
 jit_step_end(&units,&step,&s,true,&tlb);
 assert(units.n==1 && units.u[0].n==2 && units.u[0].w[0]==a && units.u[0].w[1]==b);
 assert(units.u[0].end==FUSION_WORDS && units.u[0].start==0);
 assert(s.size==FUSION_WORDS && s.jump_ip[0]==0 && s.jump_ip[1]==0);
 struct cpu_state expected,got;memset(&expected,0xa5,sizeof(expected));got=expected;
 expected.regs[a&31]=model(pc,a,b);
 gen(&s,(unsigned long)fusion_stop);fusion_call(s.block->code,&got);
 assert(!memcmp(&expected,&got,sizeof(got)));
 gen_end(&s);assert(s.block->end_addr==pc+7);free(s.block);cases++;
}
static void refusal(void) {
 uint32_t bad[]={0x91000401,0x91000420,0x11000400,0x91400400,0xb1000400,0xd1000400};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
  map(0x1000);page[0]=0x90000000;page[1]=bad[i];
  struct gen_state s={0};gen_start(0x1000,&s);assert(gen_step(&s,&tlb)==1);
  assert(s.ip==0x1004 && s.size==2);free(s.block);
 }
 // Failed peek is not an architectural fetch failure for the first instruction.
 map(0x1ffc);page[1023]=0x90000000;tlb.entries[TLB_INDEX(0x2000)].page=1;
 struct gen_state s={0};gen_start(0x1ffc,&s);misses=0;assert(gen_step(&s,&tlb)==1);
 assert(misses==1 && s.ip==0x2000 && s.size==2);
 assert(gen_step(&s,&tlb)==0 && s.ip==0x2000);
 assert(s.size==6 && s.block->code[2]==(unsigned long)gadget_set_pc &&
        s.block->code[3]==0x2000 && s.block->code[4]==(unsigned long)gadget_interrupt &&
        s.block->code[5]==INT_GPF);free(s.block);
 // Maximum unit count must fall back, not overrun the two-instruction unit array.
 map(0);page[0]=0x90000000;page[1]=0x91000000;
 struct jit_units u={.n=JIT_MAX_UNITS-1};struct jit_step step={0};
 s=(struct gen_state){.ip=8,.size=FUSION_WORDS};
 jit_step_end(&u,&step,&s,true,&tlb);
 assert(u.n==JIT_MAX_UNITS && u.u[JIT_MAX_UNITS-1].n==2);
 jit_step_end(&u,&step,&s,true,&tlb);assert(u.n==JIT_MAX_UNITS+1);
 jit_step_end(&u,&step,&s,true,&tlb);assert(u.n==JIT_MAX_UNITS+1);
}
static uint64_t ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void) {
 // Exact hot module bytes, followed by signed offset/low-page/48-bit boundaries.
 check(0xaea7c,0x90000000,0x9133a000);
 check(0xb472c,0xf0001f62,0x913a2042);
 check(0xf470c,0xd0ffffe0,0x91330000);
 const addr_t pcs[]={0,0xffc,0x1ffc,0xffffffffeffcull,0xfffffffffff8ull};
 const uint32_t imms[]={0,1,0xfffff,0x100000,0x1fffff};
 for(unsigned p=0;p<5;p++)for(unsigned i=0;i<5;i++)for(unsigned rd=0;rd<31;rd++)for(unsigned v=0;v<3;v++) {
  uint32_t imm=imms[i],a=0x90000000|((imm&3)<<29)|((imm>>2)<<5)|rd;
  uint32_t b=0x91000000|((v==0?0:v==1?1:4095)<<10)|(rd<<5)|rd;
  check(pcs[p],a,b);
 }
 refusal();
 struct gen_state s={0};map(0x1000);page[0]=0x90000000;page[1]=0x9133a000;
 gen_start(0x1000,&s);assert(gen_step(&s,&tlb));gen(&s,(unsigned long)fusion_stop);
 struct cpu_state cpu={0};uint64_t times[7];
 for(unsigned run=0;run<7;run++){uint64_t start=ns();for(unsigned j=0;j<2000000;j++)fusion_call(s.block->code,&cpu);times[run]=ns()-start;}
 printf("fusion-optim PASS cases=%u words=%u timings_ns=",cases,FUSION_WORDS);
 for(unsigned i=0;i<7;i++)printf("%s%lu",i?",":"",times[i]);puts("");free(s.block);
}

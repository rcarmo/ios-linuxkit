// Crate-inspired seeded instruction streams, independently implemented here.
// Straight-line ALU/RMW/flag dependencies with native CPU as the oracle.
static void stream_probe(void) {
 const unsigned regs[]={0,1,2,3,4,5,6,7,9,10,11,12};
 unsigned cases=0;
 for(unsigned pin=0;pin<2;pin++) for(unsigned seed=1;seed<=256;seed++) {
  pin_init(pin);pic_on=false;rng=0x936d48a1b7ull ^ seed;
  struct em e,r;init_em(&e);init_em(&r);uint32_t words[32];
  if(pin)load_pinned(&e);
  for(unsigned i=0;i<32;i++) {
   unsigned rd=regs[random64()%12],rn=regs[random64()%12],rm=regs[random64()%12];
   uint32_t sf=(random64()&1)<<31,w;
   switch(random64()%10) {
    case 0:w=sf|0x0b000000|(rm<<16)|(rn<<5)|rd;break; // add
    case 1:w=sf|0x6b000000|(rm<<16)|(rn<<5)|rd;break; // subs/NZCV
    case 2:w=sf|0x4a000000|(rm<<16)|(rn<<5)|rd;break; // eor
    case 3:w=sf|0x2a000000|(rm<<16)|(rn<<5)|rd;break; // orr
    case 4:w=sf|0x1a000000|(rm<<16)|(rn<<5)|rd;break; // adc
    case 5:w=sf|0x1a800000|(rm<<16)|((random64()%14)<<12)|(rn<<5)|rd;break; // csel
    case 6:w=sf|0x52800000|((random64()&65535)<<5)|rd;break; // movz/full write
    case 7:w=sf|0x72800000|(1<<21)|((random64()&65535)<<5)|rd;break; // movk/RMW
    case 8:w=sf|0x33000000|(sf?(1<<22):0)|(8<<16)|(15<<10)|(rn<<5)|rd;break; // bfm/RMW
    default:w=sf|0x11000000|((random64()&4095)<<10)|(rd<<5)|rd;break; // source alias
   }
   words[i]=w;bool mem;assert(emit_insn(&e,w,0x1000+4*i,i,&mem)&&!mem&&!e.fail);put(&r,w);
  }
  if(pin)for(int k=0;k<n_pinned;k++)put(&e,0xf9000000u|((greg_off(pin_guest[k])/8)<<10)|(1u<<5)|pin_host[k]);
  put(&e,0xd65f03c0);put(&r,0xd65f03c0);void *jit=executable(&e),*ref=executable(&r);
  for(unsigned input=0;input<4;input++) {
   struct cpu_state expected={0};for(unsigned i=0;i<31;i++)expected.regs[i]=input==0?0:input==1?~0ull:random64();
   expected.nzcv=(random64()&15)<<28;struct cpu_state got=expected;
   expected.nzcv=reference_call(ref,&expected,offsetof(struct cpu_state,nzcv));probe_call(jit,&got,NULL);
   if(memcmp(expected.regs,got.regs,sizeof(expected.regs))||expected.nzcv!=got.nzcv) {
    fprintf(stderr,"stream mismatch seed=%u pin=%u input=%u\n",seed,pin,input);
    for(unsigned i=0;i<32;i++)fprintf(stderr,"%08x%s",words[i],i==31?"\n":" ");abort();
   }cases++;
  }
  munmap(jit,65536);munmap(ref,65536);
 }
 // A fresh full overwrite no longer loads the dead destination; MOVK still does.
 pin_init(true);struct em e;init_em(&e);bool mem;
 assert(emit_insn(&e,0xd2800549,0x1000,0,&mem));assert(e.n==2); // MOVZ x9,#42; STR
 init_em(&e);assert(emit_insn(&e,0xf2a00549,0x1000,0,&mem));assert(e.n==3); // LDR; MOVK; STR
 puts("write-only-allocation-ok: MOVZ 3->2 words; MOVK preload retained");
 printf("seeded-stream-oracle-ok: %u streams, 32 instructions, lazy eviction/RMW/flags/aliases\n",cases);
}

static void simd_dup_probe(void) {
 pin_init(false);pic_on=false;simd_on=true;unsigned cases=0;
 const unsigned aliases[][2]={{0,1},{0,0},{31,1},{0,31},{31,31}};
 for(unsigned q=0;q<2;q++)for(unsigned imm=0;imm<32;imm++)for(unsigned a=0;a<5;a++) {
  unsigned rd=aliases[a][0],rn=aliases[a][1];uint32_t w=0x0e000400|(q<<30)|(imm<<16)|(rn<<5)|rd;
  bool valid=(imm&15)&&((imm&7)||q);struct em e;init_em(&e);bool mem;
  if(!valid){assert(!emit_insn(&e,w,0x1000,0,&mem));continue;}
  assert(emit_insn(&e,w,0x1000,0,&mem)&&!mem&&!e.fail);put(&e,0xd65f03c0);
  // Direct hardware instruction, with no emulated/remapped operand encoding.
  struct em r;init_em(&r);vload(&r,0,0);vload(&r,1,1);vload(&r,31,31);put(&r,w);
  vstore(&r,0,0);vstore(&r,1,1);vstore(&r,31,31);put(&r,0xd65f03c0);
  void *jit=executable(&e),*ref=executable(&r);
  struct cpu_state expected={0},oracle,got;
  for(unsigned v=0;v<32;v++)for(unsigned b=0;b<16;b++)expected.fp[v].b[b]=(v*17+b*13)^0xa5;
  expected.nzcv=0xa0000000;expected.fpsr=0x0800001f;oracle=got=expected;
  unsigned size=1u<<__builtin_ctz(imm),lane=imm>>(1+__builtin_ctz(imm));
  unsigned char value[8];memcpy(value,expected.fp[rn].b+lane*size,size);
  memset(expected.fp[rd].b,0,16);for(unsigned b=0;b<(q?16:8);b+=size)memcpy(expected.fp[rd].b+b,value,size);
  // Verify actual host flags as well as the guest backing fields.
  uint64_t old_fpsr,old_nzcv,after_fpsr,after_nzcv;
  __asm__ volatile("mrs %0, fpsr\n mrs %1, nzcv":"=r"(old_fpsr),"=r"(old_nzcv));
  probe_call(ref,&oracle,NULL);
  __asm__ volatile("msr fpsr, %0\n msr nzcv, %1"::"r"((uint64_t)expected.fpsr),"r"((uint64_t)expected.nzcv):"cc");
  probe_call(jit,&got,NULL);
  __asm__ volatile("mrs %0, fpsr\n mrs %1, nzcv":"=r"(after_fpsr),"=r"(after_nzcv));
  __asm__ volatile("msr fpsr, %0\n msr nzcv, %1"::"r"(old_fpsr),"r"(old_nzcv):"cc");
  assert(!memcmp(&expected,&oracle,sizeof(expected))&&!memcmp(&expected,&got,sizeof(got)));
  assert(after_fpsr==expected.fpsr&&after_nzcv==expected.nzcv);
  munmap(jit,65536);munmap(ref,65536);cases++;
 }
 printf("simd-dup-element-ok: %u legal lane/Q/alias cases, native+C oracle, invalid forms rejected\n",cases);
}

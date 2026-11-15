import { test, expect } from 'bun:test';
import { recordingSymbols } from '../../../tools/jit_aot/kit';
import { join, resolve } from 'node:path';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
const root=resolve(import.meta.dir,'../../..');
const py=(code:string)=>Bun.spawnSync(['python3','-c',`import sys; sys.path.insert(0,${JSON.stringify(join(root,'tools/jit_aot'))}); `+code],{stdout:'pipe',stderr:'pipe'});
test('both recording consumers use stub-aware validation including header symbols',()=>{
 const header={stubs:[{words:[0],rel:[[0,'sym','fiber_ret']]}]};
 expect([...recordingSymbols(header,[{segs:[{rel:[[0,'stub',1],[1,'exit']]}]}])]).toEqual(['ish_aot_register','fiber_ret']);
 expect(()=>recordingSymbols(header,[{segs:[{rel:[[0,'stub',2]]}]}])).toThrow('missing shared stub');
 expect(()=>recordingSymbols({},[{segs:[{rel:[[0,'stub',1]]}]}])).toThrow();
 expect(()=>recordingSymbols(header,[{segs:[{rel:[[0,'stub',1,'extra']]}]}])).toThrow();
 expect(()=>recordingSymbols(header,[{segs:[{words:[0],rel:[[1,'stub',1]]}]}])).toThrow('position');
 expect(py(`from gen import validate_stub_relocs; validate_stub_relocs({'stubs':[{'rel':[]}]},[{'segs':[{'words':[0],'rel':[[1,'stub',1]]}]}])`).exitCode).not.toBe(0);
});
test('shared stubs remain single-word branches in ELF and Mach-O; inline recordings accepted',()=>{
 const r=py(`from gen import emit_segment, elf_lines, validate_stub_relocs; h={'stubs':[{'words':[0x14000000], 'rel':[[0,'exit']]}]}; t={'segs':[{'words':[0x14000000], 'rel':[[0,'stub',1]], 'links':[]}]}; validate_stub_relocs(h,[t]); validate_stub_relocs({},[]); out=[]; stats={'exit':0}; assert emit_segment(out,{},0,0,t['segs'][0],stats); assert '    b Laot_stub_1' in out; assert '    b .Laot_stub_1' in list(elf_lines(out)); print('ok')`);
 expect(r.exitCode).toBe(0);
 for(const value of ['0','2','"fiber_ret"'])expect(py(`from gen import validate_stub_relocs; validate_stub_relocs({'stubs':[{'rel':[]}]},[{'segs':[{'rel':[[0,'stub',${value}]]}]}])`).exitCode).not.toBe(0);
});
test('real shared paths execute at O0/O2 with native/gadget, timer and refused-edge parity',()=>{
 const dir=mkdtempSync(join(tmpdir(),'ish-stubs-'));
 try {
  for(const opt of ['-O0','-O2']) {
   const exe=join(dir,'stubs'+opt);
   const cc=Bun.spawnSync([process.env.CC||'clang','-std=gnu11',opt,'-DGUEST_ARM64=1','-DISH_JIT=1','-I'+root,'-I'+resolve(root,process.env.BUILD_DIR||'build-arm64-native-release'),'-ffunction-sections','-fdata-sections','-Wno-unused-function','-Wl,--gc-sections','-pthread',join(import.meta.dir,'stubs.c'),join(import.meta.dir,'call.S'),'-o',exe],{stdout:'pipe',stderr:'pipe'});
   expect(cc.stderr.toString()).not.toContain('error:');expect(cc.exitCode).toBe(0);
   const run=Bun.spawnSync(['timeout','15',exe],{stdout:'pipe',stderr:'pipe'});
   expect(run.exitCode).toBe(0);expect(run.stdout.toString()).toContain('codeversion=15');
   console.log(run.stdout.toString().trim());
  }
 } finally {rmSync(dir,{recursive:true,force:true});}
},30000);
test('complete three-stub fixture generates and assembles for ELF and Mach-O',async()=>{
 const dir=mkdtempSync(join(tmpdir(),'ish-stub-image-'));
 try {
  await Bun.write(join(dir,'module'),'guest module bytes');
  await Bun.write(join(dir,'host.c'),'void ish_aot_register(void *p) {} void fiber_ret(void) {}\n');
  const host=join(dir,'host.o');
  expect(Bun.spawnSync(['clang','-c',join(dir,'host.c'),'-o',host]).exitCode).toBe(0);
  const header={abi:0x12345678,prologue_words:1,entry_off:4,n_pinned:1,exit_stub:[0xd61f0100],stubs:[
   {words:[0x14000000],rel:[[0,'exit']]},
   {words:[0xd2800008,0xf2a00008,0xf2c00008,0xf2e00008,0x14000000],rel:[[0,'sym','fiber_ret'],[4,'exit']]},
   {words:[0x14000000],rel:[[0,'stub',1]]}]};
  const row={mod:'/module',off:16,idx:0,key:[0,16,0,0,1,0,0,0,0,0,0,0,0,0,0,0],keysym:{},segs:[{pos:0,words:[0x14000000,0x14000000,0x14000000],rel:[[0,'stub',1],[1,'stub',2],[2,'stub',3]],links:[],loop:null}]};
  const rec=join(dir,'record.jsonl');await Bun.write(rec,JSON.stringify({header})+'\n'+JSON.stringify(row)+'\n');
  for(const format of ['elf','macho']) {
   const asm=join(dir,format+'.S'),obj=join(dir,format+'.o');
   const gen=Bun.spawnSync(['python3',join(root,'tools/jit_aot/gen.py'),rec,host,dir,asm,'--format',format],{stdout:'pipe',stderr:'pipe'});
   expect(gen.stderr.toString()).toBe('');expect(gen.exitCode).toBe(0);
   const compile=Bun.spawnSync(['clang',...(format==='macho'?['-target','arm64-apple-macos11']:[]),'-c',asm,'-o',obj],{stdout:'pipe',stderr:'pipe'});
   expect(compile.stderr.toString()).toBe('');expect(compile.exitCode).toBe(0);
   const text=readFileSync(asm,'utf8');for(const id of [1,2,3])expect(text).toContain(`Laot_stub_${id}:`);
  }
 } finally {rmSync(dir,{recursive:true,force:true});}
});
test('profile CLI normalizes workloads equally and reports unmatched executed offsets',async()=>{
 const dir=mkdtempSync(join(tmpdir(),'ish-hotblocks-'));
 try {
  const rec=join(dir,'record'),a=join(dir,'a'),b=join(dir,'b'),keep=join(dir,'keep');
  await Bun.write(rec,[{header:{}},{mod:'/lib/test with spaces.so',off:16,segs:[{words:[0]}]},{mod:'/lib/test with spaces.so',off:32,segs:[{words:[0]}]}].map(x=>JSON.stringify(x)).join('\n')+'\n');
  await Bun.write(a,'# profile\n/lib/test with spaces.so 10 1 1000\n/lib/test with spaces.so 30 1 1000\n');
  await Bun.write(b,'/lib/test with spaces.so 20 1 1\n');
  const run=Bun.spawnSync(['python3',join(root,'tools/jit_aot/hotblocks.py'),rec,a,b,'--coverage','.6','--keep',keep],{stdout:'pipe',stderr:'pipe'});
  expect(run.exitCode).toBe(0);expect(readFileSync(keep,'utf8')).toBe('0x20\n');
  expect(run.stdout.toString()).toContain('75.000%');
 } finally {rmSync(dir,{recursive:true,force:true});}
});
test('profile selection ranks saved instructions per byte, rejects empty/invalid coverage',()=>{
 const r=py(`from hotblocks import select; assert select({16:400,32:4},{16:0.9,32:0.1},0.1)[0]==[32]; assert select({16:400,32:4},{16:0.9,32:0.1},1)[0]==[16,32]; print('ok')`);
 expect(r.exitCode).toBe(0);
 for(const coverage of ['0','1.1','float("nan")'])expect(py(`from hotblocks import select; select({1:4},{1:1},${coverage})`).exitCode).not.toBe(0);
 expect(py('from hotblocks import select; select({1:4},{2:1},0.9)').exitCode).not.toBe(0);
});

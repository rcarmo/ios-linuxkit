import { test, expect } from 'bun:test';
import { readFileSync, mkdirSync, writeFileSync, existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
const root=resolve(import.meta.dir,'../../..');
const out='/workspace/tmp/ish-fusion-optim-build';
const build=resolve(process.env.BUILD_DIR || join(root,'build-arm64-native-release'));
test('production gen + real gadget differential, refusal, unit bounds and benchmark',()=>{
 mkdirSync(out,{recursive:true});
 const source=process.env.FUSION_GEN_SOURCE || join(root,'asbestos/guest-arm64/gen.c');
 const gen=readFileSync(source,'utf8');
 writeFileSync(join(out,'cpu-offsets.h'),readFileSync(join(build,'cpu-offsets.h')));
 const bodies=[['math.S','adr'],['memory.S','fused_adrp_add']] as const;
 let asm='#include "asbestos/guest-arm64/gadgets-aarch64/gadgets.h"\n.text\n';
 for(const [file,name] of bodies){
  const s=readFileSync(join(root,'asbestos/guest-arm64/gadgets-aarch64',file),'utf8');
  const start=s.indexOf('.gadget '+name+'\n'),end=s.indexOf('\n/*',start);
  if(start<0||end<0)throw Error('gadget boundaries');asm+=s.slice(start,end)+'\n';
 }
 writeFileSync(join(out,'gadget-bodies.S'),asm);
 const symbols=[...gen.matchAll(/extern void (gadget_\w+)\(void\);/g)].map(m=>m[1]);
 writeFileSync(join(out,'unused-gadgets.c'),'#include <stdlib.h>\n'+[...new Set(symbols)].filter(s=>s!=='gadget_adr'&&s!=='gadget_fused_adrp_add').map(s=>`void ${s}(void){abort();}`).join('\n'));
 for(const opt of ['-O0','-O2']) {
  const exe=join(out,'fusion'+(process.env.FUSION_WORDS||'2')+opt);
  const cc=Bun.spawnSync([process.env.CC||'clang','-std=gnu11',opt,'-DGUEST_ARM64=1','-DISH_JIT=1','-DFUSION_WORDS='+(process.env.FUSION_WORDS||'2'),'-DFUSION_GEN_SOURCE='+JSON.stringify(source),'-I'+root,'-I'+out,'-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wno-unused-function','-pthread',join(import.meta.dir,'fusion-optim.c'),join(import.meta.dir,'fusion-optim-call.S'),join(out,'gadget-bodies.S'),join(out,'unused-gadgets.c'),'-o',exe],{stdout:'pipe',stderr:'pipe'});
  if(cc.exitCode!==0 || cc.signalCode)throw Error(cc.stderr.toString());
  const r=Bun.spawnSync(['timeout','15',exe],{stdout:'pipe',stderr:'pipe'});
  if(r.exitCode!==0 || r.signalCode)throw Error(`exit ${r.exitCode}: ${r.stdout} ${r.stderr}`);
  expect(r.stdout.toString()).toContain('fusion-optim PASS cases=2328');console.log(opt,r.stdout.toString().trim());
 }
},60000);

const modules='/workspace/artifacts/ish-heldout-linux-20261115-refresh/rootfs/data';
(existsSync(modules) ? test : test.skip)('representative hot bytes come from exact training modules and ordinary dispatch rows',()=>{
 const mod='/usr/lib/libpython3.14.so.1.0';
 const bytes=readFileSync(modules+mod);
 const profile=readFileSync('/workspace/artifacts/ish-portable-policy-20261115/profile-training/final.profile','utf8');
 for(const [off,a,b,entry,n,count] of [
  [0xaea7c,0x90000000,0x9133a000,'aea60',11,12196],
  [0xb472c,0xf0001f62,0x913a2042,'b4720',10,11929],
  [0xf470c,0xd0ffffe0,0x91330000,'f46f0',11,9286],
 ] as const){
  expect(bytes.readUInt32LE(off)).toBe(a);expect(bytes.readUInt32LE(off+4)).toBe(b);
  expect(profile).toContain(`${mod} ${entry} ${n} ${count}\n`);
 }
});

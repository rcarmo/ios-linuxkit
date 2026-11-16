import {test,expect} from 'bun:test';
import {select,profile,generate,verifyInputs} from '../../../tools/jit_aot/select';
import {createHash} from 'node:crypto';
import {mkdtempSync,writeFileSync,symlinkSync,unlinkSync,rmSync,realpathSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
const t=(off:number,bytes:number,entries=[off])=>({off,bytes,entries});
test('actual producer format: hex file offsets, instruction/dispatch counts, anonymous rows',()=>{
 const p=profile('# dispatch profile; dropped=0; ordinary build\n/usr/lib/x 10 2 50\n/usr/lib/x 10 1 5\nabc 3 900\n/lib/a module.so a0 4 7\n');
 expect(p.get('/usr/lib/x')?.get(16)).toBe(55);expect(p.get('/lib/a module.so')?.get(160)).toBe(7);expect(p.size).toBe(2);
 for(const s of ['{}','# dispatch profile; dropped=1;','# dispatch profile; dropped=0;\nbad','# dispatch profile; dropped=0;\n/x 1 1 9007199254740992'])expect(()=>profile(s)).toThrow();
});
test('density ranking and strict byte budget preserve hot translations without evaluation',()=>{
 const r=[t(16,40),t(32,60),t(48,100)];
 const s=select(r,new Map([[16,90],[32,9],[48,1]]),.9,.5);
 expect(s.offsets).toEqual([16]);expect(s.nativeBytes).toBe(40);expect(s.representedFraction).toBe(.9);
 expect(s.totalTranslations).toBe(3);
 const missing=select(r,new Map([[16,90],[1000,810]]),.9,.5);
 expect(missing.representedFraction).toBe(1);expect(missing.representedModuleFraction).toBe(.1);
});
test('keep closure accounts overlapping and duplicate variants, not just first offsets',()=>{
 const r=[t(16,20,[16,32]),t(32,30),t(16,1000),t(48,100)];
 const s=select(r,new Map([[32,100]]),1,.05);
 expect(s.offsets).toEqual([32]);expect(s.translations).toBe(2);expect(s.nativeBytes).toBe(50);
 expect(s.representedWeight).toBe(100);
});
test('untrained modules remain explicitly unrepresented; impossible/invalid budgets fail',()=>{
 const r=[t(16,20),t(32,100)];const s=select(r,new Map(),1,.5);
 expect(s.translations).toBe(1);expect(s.representedFraction).toBe(0);
 for(const [coverage,ratio] of [[0,.5],[1,0],[1,1.1],[NaN,.5],[1,.01]])expect(()=>select(r,new Map(),coverage,ratio)).toThrow();
 expect(()=>select([t(Number.MAX_SAFE_INTEGER+1,1)],new Map(),1,1)).toThrow();
});
test('publication guard rejects input mutation and same-content alias retargeting',async()=>{
 const dir=mkdtempSync(join(tmpdir(),'aot-selection-input-'));
 try {
  const a=join(dir,'a'),b=join(dir,'b'),alias=join(dir,'alias');
  writeFileSync(a,'training');writeFileSync(b,'training');symlinkSync(a,alias);
  const fixed=[{path:realpathSync(a),requestedPath:alias,sha256:createHash('sha256').update('training').digest('hex')}];
  await expect(verifyInputs(fixed)).resolves.toBeUndefined();
  writeFileSync(a,'evaluation');await expect(verifyInputs(fixed)).rejects.toThrow('input changed');
  writeFileSync(a,'training');unlinkSync(alias);symlinkSync(b,alias);
  await expect(verifyInputs(fixed)).rejects.toThrow('input changed');
 }finally{rmSync(dir,{recursive:true});}
});
test('CLI rejects ambiguous paths and options before writing',async()=>{
 await expect(generate(['p','m','b','r','o'])).rejects.toThrow('absolute');
 await expect(generate(['/p','/m','/b','/r','/o','--evaluate=anything'])).rejects.toThrow('unknown');
});

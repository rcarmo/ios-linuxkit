import {test,expect} from 'bun:test';
import {mkdtempSync,writeFileSync,rmSync,symlinkSync,unlinkSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {ABI,CODE_VERSION,assertContract,assertOracle,assertDiagnostics,cleanEnv,digest,fileHash,verifyFiles,freeze} from './heldout';
test('current ABI only; stale manifest rejected before file access',()=>{
 expect(()=>assertContract(ABI,CODE_VERSION)).not.toThrow();
 expect(()=>assertContract('d5483d56',15)).toThrow('stale');
 expect(()=>assertContract(ABI,13)).toThrow('stale');
 expect(()=>freeze({abi:'d5483d56',codeVersion:15} as any)).toThrow('stale');
});
test('oracles reject wrong bytes and missing newline',()=>{
 assertOracle('ok\n','ok\n');
 expect(()=>assertOracle('bad\n','ok\n')).toThrow();
 expect(()=>assertOracle('ok','ok\n')).toThrow();
});
test('polluted environment stripped, default gadget remains no JIT',()=>{
 const source={PATH:'/bin',ISH_JIT:'1',ISH_JIT_STUBS:'0',ISH_AOT_FAMILY:'1',ISH_BLOCK_PROF_FILE:'/tmp/polluted',ISH_BLOCK_PROF:'1',ISH_MEMORY_PRESSURE:'1',ISH_ARM64_FUSION_STATS:'1'};
 const env=cleanEnv('gadget',source);
 expect(env).toEqual({PATH:'/bin',ISH_JIT:'0',ISH_AOT_FAMILY:'0',ISH_JIT_STATS:'1'});
 expect(source.ISH_JIT).toBe('1');
 expect(cleanEnv('linked-no-emitter',source).ISH_JIT).toBe('1');
});
test('manifest mismatch rejected; frozen hash cannot change with input bytes',()=>{
 const dir=mkdtempSync(join(tmpdir(),'heldout-'));
 try { const p=join(dir,'input');writeFileSync(p,'before');
 const hashes=[fileHash(p)];const frozen=JSON.stringify(hashes),hash=digest(frozen);
 verifyFiles(hashes);writeFileSync(p,'after');
 expect(()=>verifyFiles(hashes)).toThrow('manifest hash mismatch');
 expect(digest(frozen)).toBe(hash);expect(hashes[0].sha256).toBe(digest('before'));
 } finally {rmSync(dir,{recursive:true});}
});
test('retargeted aliases cannot verify a different executed input',()=>{
 const d=mkdtempSync(join(tmpdir(),'ish-heldout-alias-'));
 try {
  writeFileSync(join(d,'a'),'same bytes'); writeFileSync(join(d,'b'),'same bytes');
  symlinkSync(join(d,'a'),join(d,'input')); const frozen=fileHash(join(d,'input'));
  verifyFiles([frozen]); unlinkSync(join(d,'input')); symlinkSync(join(d,'b'),join(d,'input'));
  expect(()=>verifyFiles([frozen])).toThrow('manifest hash mismatch');
 } finally { rmSync(d,{recursive:true}); }
});
test('timeout terminates the measured command process group',()=>{
 const d=mkdtempSync(join(tmpdir(),'ish-heldout-timeout-'));
 try {
  const result=Bun.spawnSync(['/usr/bin/timeout','-k','1','0.1s','/bin/sh','-c',
   '(sleep 0.3; echo orphan > "$1") & wait','sh',join(d,'orphan')],{stdout:'pipe',stderr:'pipe'});
  expect(result.exitCode).toBe(124);
  Bun.sleepSync(400);
  expect(()=>fileHash(join(d,'orphan'))).toThrow();
 } finally { rmSync(d,{recursive:true}); }
});
test('no-emitter evidence requires positive hits and zero code',()=>{
 assertDiagnostics('linked-no-emitter','AOT 23 (5 images); segments 0, units 0, code 0 KB',5);
 expect(()=>assertDiagnostics('linked-no-emitter','AOT 0 (5 images); segments 0, units 0, code 0 KB',5)).toThrow();
 expect(()=>assertDiagnostics('linked-no-emitter','AOT 23 (5 images); segments 1, units 2, code 3 KB',5)).toThrow();
 expect(()=>assertDiagnostics('linked-no-emitter','AOT 23 (4 images); segments 0, units 0, code 0 KB',5)).toThrow();
});

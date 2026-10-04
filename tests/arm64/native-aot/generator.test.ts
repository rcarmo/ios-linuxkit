import { test, expect } from 'bun:test';
import { mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
const root=resolve(import.meta.dir,'../../..');
const dir=mkdtempSync(join(tmpdir(),'ish-aot-generator-'));
process.on('exit',()=>rmSync(dir,{recursive:true,force:true}));
function python(code:string,...args:string[]){return Bun.spawnSync(['python3','-c',`import sys; sys.path.insert(0,${JSON.stringify(join(root,'tools/jit_aot'))}); `+code,...args],{stdout:'pipe',stderr:'pipe'});}
test('ELF conversion keeps exact-size relocations, local labels and compact table sections',()=>{
 const r=python(`from gen import elf_lines; print('\\n'.join(elf_lines([
 '.section __TEXT,__ish_aot,regular,pure_instructions', 'Laot_text_start:',
 'Lt2s1_l0:', 'adrp x8, gadget_load64@PAGE', 'add x8, x8, gadget_load64@PAGEOFF',
 '.section __DATA,__const', '.long Lt2s1_l0 - .', '.globl _ish_aot_module_musl',
 '_ish_aot_module_musl:', '.quad Laot_text_start',
 '.section __DATA,__mod_init_func,mod_init_funcs', '.quad Laot_register',
 'b _ish_aot_register'])))`);
 expect(r.exitCode).toBe(0);const s=r.stdout.toString();
 expect(s).toContain('.section .text.ish_aot,"ax",@progbits');
 expect(s).toContain('.section .data.rel.ro.ish_aot,"aw",@progbits');
 expect(s).toContain('.long .Lt2s1_l0 - .');expect(s).toContain('add x8, x8, :lo12:gadget_load64');
 expect(s).toContain('.init_array');expect(s).toContain('.globl ish_aot_module_musl');
 expect(s).toContain('b ish_aot_register');expect(s).not.toContain('@PAGE');
 expect(s).toContain('.note.GNU-stack');
});
test('invalid and empty recordings fail closed without an image',async()=>{
 for(const [name,text] of Object.entries({empty:'',missing:'{"mod":"/lib/x"}\n',bad:'not json\n',zero:'{"header":{"abi":123}}\n'})){
  const record=join(dir,name+'.jsonl'),out=join(dir,name+'.S');await Bun.write(record,text);
  const r=Bun.spawnSync(['python3',join(root,'tools/jit_aot/gen.py'),record,'/missing-ish','/missing-root',out,'--format','elf'],{stdout:'pipe',stderr:'pipe'});
  expect(r.exitCode).not.toBe(0);expect(await Bun.file(out).exists()).toBe(false);
 }
});
test('targeted recorder propagates failures and never publishes a manifest',async()=>{
 const ish=join(dir,'failing-ish');await Bun.write(ish,'#!/bin/sh\nprintf "%s\\n" "$@"\necho workload-failed >&2\nexit 37\n');
 Bun.spawnSync(['chmod','+x',ish]);const out=join(dir,'failed');
 const r=Bun.spawnSync(['bun',join(root,'tools/jit_aot/targeted.ts'),ish,dir,out],{stdout:'pipe',stderr:'pipe'});
 expect(r.exitCode).not.toBe(0);expect(r.stderr.toString()).toContain('exit 37');
 expect(await Bun.file(join(out,'manifest.json')).exists()).toBe(false);
 const inventory=readFileSync(join(out,'inventory.log'),'utf8');
 expect(inventory).toContain('workload-failed');
 expect(inventory).toContain('apk list --installed musl busybox python3 zlib');
 expect(inventory).not.toContain('apk info -v');
});

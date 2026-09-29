#!/usr/bin/env bun
/** Hardware-breakpoint proof for each linked image. Requires a debug no-emitter build. */
import {mkdirSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {cleanEnv} from '../../../tools/jit_aot/kit';
const [binaryArg,rootArg,outArg]=process.argv.slice(2);
if(!outArg)throw Error('execution.ts NO_EMIT_DEBUG_BINARY RESTORED_ROOT OUTPUT');
const binary=resolve(binaryArg),root=resolve(rootArg),out=resolve(outArg);mkdirSync(out,{recursive:false});
const cases=[['musl',0,'echo musl-ok'],['busybox',1,'i=0; while [ "$i" -lt 100 ]; do i=$((i+1)); done; echo busybox-ok'],['python',2,`python3 -c 'assert sum(range(10000))==49995000; print("python-ok")'`],['zlib',3,`python3 -c 'import zlib; a=b"x"*100000; assert zlib.decompress(zlib.compress(a))==a; print("zlib-ok")'`]] as const;
for(const [name,index,command] of cases){
 const script=`set pagination off
set confirm off
set print thread-events off
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
break aot_install if t >= aot_images[${index}]->trans && t < aot_images[${index}]->trans + aot_images[${index}]->ntrans
commands
silent
set $s = (struct aot_seg *)((char *)&t->seg + t->seg)
set $c = (char *)&$s->code + $s->code
set $lo = aot_images[${index}]->text_start
set $hi = aot_images[${index}]->text_end
printf "IMAGE ${name} start=%p end=%p code=%p\\n", $lo, $hi, $c
disable 1
hbreak *$c
commands
silent
printf "ACTUAL_AOT_PC ${name} pc=%p region=%p\\n", $pc, region
if $pc < $lo || $pc >= $hi || region != 0
quit 1
end
disable 2
continue
end
continue
end
run
if $_exitcode != 0
quit 1
end
quit
`;
 const path=join(out,name+'.gdb');await Bun.write(path,script);
 const r=Bun.spawnSync(['timeout','-k','3','120','gdb','-q','-batch','-x',path,'--args',binary,'-f',root,'/bin/sh','-ec',command],{env:cleanEnv({ISH_JIT:'1',ISH_AOT_FAMILY:'0',ISH_JIT_STATS:'1'}),stdout:'pipe',stderr:'pipe',maxBuffer:8*1024*1024});
 const log=r.stdout.toString()+r.stderr.toString();await Bun.write(join(out,name+'.log'),log);
 if(r.exitCode!==0||!log.includes(`ACTUAL_AOT_PC ${name}`)||log.includes('Error in testing breakpoint condition')||!log.includes(name+'-ok')||!/segments 0, units 0, code 0 KB/.test(log))throw Error(`${name}: execution proof failed, exit ${r.exitCode}; see ${out}`);
 console.log(name+': native PC hit, no emitter, normal exit');
}

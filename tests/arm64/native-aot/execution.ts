#!/usr/bin/env bun
/** Hardware-breakpoint proof for each linked image. Requires a debug no-emitter build. */
import {mkdirSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {cleanEnv} from './heldout';
export function executionScript(name:string) {
 if(!/^[a-z_][a-z_0-9]*$/.test(name))throw Error('invalid module symbol');
 const module=`((struct aot_module *)&ish_aot_module_${name})`;
 const table=`${module}->trans`;
 return `set pagination off
set confirm off
set print thread-events off
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
break aot_install if t >= ${table} && t < (${table}) + ${module}->ntrans
commands
silent
set $m = ${module}
set $s = (struct aot_seg *)((char *)&t->seg + t->seg)
set $c = (char *)&$s->code + $s->code
set $lo = $m->text_start
set $hi = $m->text_end
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
}
export async function main(args:string[]) {
 const [binaryArg,rootArg,outArg,option]=args;
 if(!outArg||args.length>4||option&&option!=='--bun')throw Error('execution.ts NO_EMIT_DEBUG_BINARY RESTORED_ROOT NEW_OUTPUT [--bun]');
 const binary=resolve(binaryArg),root=resolve(rootArg),out=resolve(outArg);mkdirSync(out,{recursive:false});
 const cases=[['musl','echo musl-ok'],['busybox','i=0; while [ "$i" -lt 100 ]; do i=$((i+1)); done; echo busybox-ok'],['python',`python3 -c 'assert sum(range(10000))==49995000; print("python-ok")'`],['zlib',`python3 -c 'import zlib; a=b"x"*100000; assert zlib.decompress(zlib.compress(a))==a; print("zlib-ok")'`]];
 if(option==='--bun')cases.push(['bun',`/usr/local/bin/bun -e 'if (Array.from({length:1000},(_,i)=>i).reduce((a,b)=>a+b,0)!==499500) throw Error("sum"); console.log("bun-ok")'`]);
 for(const [name,command] of cases){
 const path=join(out,name+'.gdb');await Bun.write(path,executionScript(name));
 const r=Bun.spawnSync(['timeout','-k','3','120','gdb','-q','-batch','-x',path,'--args',binary,'-f',root,'/bin/sh','-ec',command],{env:cleanEnv('linked-no-emitter'),stdout:'pipe',stderr:'pipe',maxBuffer:8*1024*1024});
 const log=r.stdout.toString()+r.stderr.toString();await Bun.write(join(out,name+'.log'),log);
 if(r.exitCode!==0||!log.includes(`ACTUAL_AOT_PC ${name}`)||log.includes('Error in testing')||!log.includes(name+'-ok')||!/segments 0, units 0, code 0 KB/.test(log))throw Error(`${name}: execution proof failed, exit ${r.exitCode}; see ${out}`);
 console.log(name+': native PC hit, no emitter, normal exit');
 }
}
if(import.meta.main)await main(process.argv.slice(2));

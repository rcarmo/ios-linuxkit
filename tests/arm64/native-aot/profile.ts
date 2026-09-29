#!/usr/bin/env bun
/** Fresh-process timing/RSS plus optional Linux perf sampling; verified outputs. */
import {resolve,join} from 'node:path';import {mkdirSync,statSync} from 'node:fs';
import {cleanEnv} from '../../../tools/jit_aot/kit';
const [g,a,r,o]=process.argv.slice(2);if(!o)throw Error('profile.ts GADGET AOT ROOT OUTPUT');
const bins={gadget:resolve(g),aot:resolve(a)},root=resolve(r),out=resolve(o);mkdirSync(out,{recursive:false});
const cpu=process.env.PERF_CPU||'0';const n=5;
const launcher=join(out,'resource');const cc=Bun.spawnSync(['clang','-O2','-Wall','-Wextra','-Werror',join(import.meta.dir,'resource.c'),'-o',launcher],{stdout:'pipe',stderr:'pipe'});if(cc.exitCode)throw Error(cc.stderr.toString());
const cases={startup:'echo startup-ok',python:`python3 -c 'assert sum(i*i for i in range(200000))==2666646666700000; print("python-ok")'`,shell:'i=0; s=0; while [ "$i" -lt 20000 ]; do s=$((s+i)); i=$((i+1)); done; test "$s" = 199990000; echo shell-ok',zlib:`python3 -c 'import zlib; a=bytes(range(256))*4096; [(zlib.decompress(zlib.compress(a))==a) or (_ for _ in ()).throw(AssertionError()) for i in range(20)]; print("zlib-ok")'`};
const result:any={cpu,runs:n,clock:'wall, includes startup; warm page cache; alternating fresh processes; schedutil',maxRSS:'bytes from wait4 in small exec launcher, excludes inherited Bun fork high-water; includes waited CLI descendants',binaryBytes:Object.fromEntries(Object.entries(bins).map(([k,p])=>[k,statSync(p).size])),cases:{}};
for(const [name,cmd] of Object.entries(cases)){
 const rows:any={gadget:[],aot:[]};
 for(let i=-1;i<n;i++)for(const mode of (i%2?['aot','gadget']:['gadget','aot']) as ('aot'|'gadget')[]){
  const t=performance.now();const p=Bun.spawnSync([launcher,'taskset','-c',cpu,bins[mode],'-f',root,'/bin/sh','-ec',cmd+'; cat /proc/ish/jit 2>/dev/null || true'],{env:cleanEnv({ISH_JIT_STATS:'1',ISH_AOT_FAMILY:'0'}),stdout:'pipe',stderr:'pipe',timeout:120000});
  const wall=performance.now()-t,s=p.stdout.toString(),err=p.stderr.toString();await Bun.write(join(out,`${name}-${mode}-${i}.log`),s+err);
  if(p.exitCode||!s.split('\n').includes(name+'-ok'))throw Error(`${name}/${mode} incorrect output/exit`);
  if(mode==='aot'&&(!/images: 4 in use, 0 rejected/.test(s)||!/segments 0, units 0, code 0 KB/.test(err)))throw Error('missing no-emitter/image proof');
  const usage=JSON.parse(err.match(/^RESOURCE (.+)$/m)?.[1]||'null');if(!usage)throw Error('missing resource measurement');
  if(i>=0)rows[mode].push({wallMs:wall,...usage});
 }
 const med=(xs:number[])=>[...xs].sort((a,b)=>a-b)[Math.floor(xs.length/2)];const summary:any={};for(const k of ['gadget','aot'])summary[k]={wallMs:med(rows[k].map((x:any)=>x.wallMs)),maxRSS:med(rows[k].map((x:any)=>x.maxRSS)),cpuUs:med(rows[k].map((x:any)=>x.cpuUs))};
 result.cases[name]={samples:rows,median:summary,speedup:summary.gadget.wallMs/summary.aot.wallMs};console.log(name+': '+JSON.stringify(result.cases[name].median)+' speedup='+result.cases[name].speedup.toFixed(3));
}
await Bun.write(join(out,'results.json'),JSON.stringify(result,null,2)+'\n');
// Sampling is diagnostic only and never mixed with wall-time samples.
for(const mode of ['gadget','aot'] as const){
 const cmd=`python3 -c 'assert sum(i*i for i in range(2000000))==2666664666667000000; print("profile-ok")'`;
 const p=Bun.spawnSync(['perf','record','-e','cpu-clock:u','-F','199','-g','-o',join(out,mode+'.perf.data'),'--','taskset','-c',cpu,bins[mode],'-f',root,'/bin/sh','-ec',cmd],{env:cleanEnv(),stdout:'pipe',stderr:'pipe',timeout:120000});
 const log=p.stdout.toString()+p.stderr.toString();await Bun.write(join(out,mode+'-perf.log'),log);
 if(p.exitCode||!log.includes('profile-ok'))throw Error(`perf ${mode} failed`);
 const q=Bun.spawnSync(['perf','report','--stdio','--no-children','--percent-limit','0.5','-i',join(out,mode+'.perf.data')],{stdout:'pipe',stderr:'pipe',timeout:120000});await Bun.write(join(out,mode+'-perf-report.txt'),q.stdout.toString()+q.stderr.toString());if(q.exitCode)throw Error('perf report failed');
}

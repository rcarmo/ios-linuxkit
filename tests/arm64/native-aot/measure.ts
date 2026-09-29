#!/usr/bin/env bun
// Fresh process A/B: no JIT emitter in AOT binary; same rootfs, pinned host CPU.
import { resolve } from 'node:path';
import { mkdirSync } from 'node:fs';
const [baselineArg, aotArg, rootArg, outArg] = process.argv.slice(2);
if (!outArg) throw new Error('usage: measure.ts GADGET_BIN AOT_BIN FAKEFS OUTPUT');
const baseline=resolve(baselineArg), aot=resolve(aotArg), root=resolve(rootArg), out=resolve(outArg);
mkdirSync(out,{recursive:true});
const cpu=process.env.PERF_CPU || '0', runs=Number(process.env.PERF_RUNS || 5);
if (!Number.isInteger(runs) || runs<3 || runs>30) throw new Error('PERF_RUNS must be 3..30');
const env=Object.fromEntries(Object.entries(process.env).filter(([k,v])=>v!==undefined&&!k.startsWith('ISH_JIT')&&!k.startsWith('ISH_AOT'))) as Record<string,string>;
const cases={
 shell:'i=0; s=0; while [ "$i" -lt 20000 ]; do s=$((s+i)); i=$((i+1)); done; test "$s" = 199990000; echo shell-ok',
 python:`python3 -c 's=sum(i*i for i in range(200000)); assert s==2666646666700000; print("python-ok")'`,
 zlib:`python3 -c 'import zlib; a=bytes(range(256))*4096; b=zlib.compress(a); assert len(b)==4396; [(zlib.decompress(zlib.compress(a))==a) or (_ for _ in ()).throw(AssertionError()) for i in range(20)]; print("zlib-ok")'`,
};
const result:any={cpu,runs,baseline,aot,root,note:'Fresh processes, warm host filesystem cache, schedutil; includes startup. No device speed claim.',cases:{}};
for(const [name,command] of Object.entries(cases)){
 const samples:Record<string,number[]>={gadget:[],aot:[]};let oracle='';
 // First matched pair warms host page cache; not part of measurements.
 for(let round=-1;round<runs;round++) for(const mode of round%2 ? ['aot','gadget'] : ['gadget','aot']){
  const start=performance.now();
  const r=Bun.spawnSync(['timeout','-k','3','120','taskset','-c',cpu,mode==='aot'?aot:baseline,'-f',root,'/bin/sh','-ec',command+'; cat /proc/ish/jit 2>/dev/null || true'],{env:{...env,ISH_JIT_STATS:'1'},stdout:'pipe',stderr:'pipe'});
  const ms=performance.now()-start,text=r.stdout.toString(),err=r.stderr.toString();
  await Bun.write(`${out}/${name}-${mode}-${round}.log`,text+err);
  if(r.exitCode!==0)throw Error(`${name}/${mode}: exit ${r.exitCode}`);
  const value=text.split('\n')[0];if(!oracle)oracle=value;if(value!==oracle)throw Error(`${name}: output mismatch`);
  if(mode==='aot'&&(!/images: 4 in use, 0 rejected/.test(text)||!/AOT installs: [1-9]/.test(text)||!/segments 0, units 0, code 0 KB/.test(err)))throw Error('missing AOT-only evidence');
  if(round>=0)samples[mode].push(ms);
 }
 const median=(xs:number[])=>[...xs].sort((a,b)=>a-b)[Math.floor(xs.length/2)];
 const g=median(samples.gadget),a=median(samples.aot);
 result.cases[name]={samples,medianMs:{gadget:g,aot:a},speedup:g/a,output:oracle};
 console.log(`${name}: gadget=${g.toFixed(1)}ms AOT=${a.toFixed(1)}ms speedup=${(g/a).toFixed(3)}x`);
}
await Bun.write(`${out}/results.json`,JSON.stringify(result,null,2)+'\n');

#!/usr/bin/env bun
/** Diagnostic user-cycle sampling, never mixed with unprofiled wall/RSS runs.
 * phase-profile.ts BREAK_EVEN_CONFIG NEW_OUT [COUNT=64]
 * Inputs/rootfs must already be staged. No training, selection, or staging here.
 */
import {readFileSync,mkdirSync,writeFileSync,realpathSync} from 'node:fs';
import {join,isAbsolute} from 'node:path';
import {freeze,oracle,parsePhases,type Phase} from './break-even';
import {ABI,fileHash,verifyFiles,cleanEnv,assertOracle,assertDiagnostics,digest} from './heldout';
export type Range={name:string;lo:number;hi:number};
function safe(n:bigint){const v=Number(n);if(!Number.isSafeInteger(v)||v<0)throw Error('unsafe ELF address');return v;}
export function elfLayout(b:Buffer,symbols:string) {
 if(b.length<64||b.subarray(0,6).toString('hex')!=='7f454c460201'||b.readUInt16LE(18)!==183)throw Error('expected little-endian ELF64 AArch64');
 const ph=safe(b.readBigUInt64LE(32)),ps=b.readUInt16LE(54),pn=b.readUInt16LE(56);
 if(ps<56||ph+ps*pn>b.length)throw Error('bad ELF program headers');
 const loads=[];
 for(let i=0;i<pn;i++){const p=ph+i*ps;if(b.readUInt32LE(p)!==1)continue;
  const off=safe(b.readBigUInt64LE(p+8)),va=safe(b.readBigUInt64LE(p+16)),size=safe(b.readBigUInt64LE(p+32)),flags=b.readUInt32LE(p+4);
  if(off+size>b.length)throw Error('truncated ELF load');loads.push({off,va,size,flags});}
 const at=(va:number,n=8)=>{const s=loads.find(s=>va>=s.va&&va+n<=s.va+s.size);if(!s)throw Error('ELF VA outside file');return s.off+va-s.va;};
 const rel=new Map<number,number>(),sh=safe(b.readBigUInt64LE(40)),ss=b.readUInt16LE(58),sn=b.readUInt16LE(60);
 if(sn&&(ss<64||sh+ss*sn>b.length))throw Error('bad ELF sections');
 for(let i=0;i<sn;i++){const h=sh+i*ss;if(b.readUInt32LE(h+4)!==4)continue;
  const off=safe(b.readBigUInt64LE(h+24)),size=safe(b.readBigUInt64LE(h+32)),ent=safe(b.readBigUInt64LE(h+56));
  if(ent<24||size%ent||off+size>b.length)throw Error('bad ELF RELA');
  for(let p=off;p<off+size;p+=ent)if(Number(b.readBigUInt64LE(p+8)&0xffffffffn)===1027)rel.set(safe(b.readBigUInt64LE(p)),safe(b.readBigInt64LE(p+16)));}
 const pointer=(va:number)=>rel.get(va)??safe(b.readBigUInt64LE(at(va)));
 const ranges:Range[]=[];
 for(const line of symbols.split('\n')){const m=line.match(/^([0-9a-f]+)\s+\w\s+ish_aot_module_([a-z_][a-z_0-9]*)$/);if(!m)continue;
  const va=parseInt(m[1],16);
  if(b.readUInt32LE(at(va+76,4))!==parseInt(ABI,16))throw Error('AOT descriptor ABI mismatch');
  const lo=pointer(va+80),hi=pointer(va+88);
  if(lo>=hi||!loads.some(s=>(s.flags&1)&&lo>=s.va&&hi<=s.va+s.size))throw Error('invalid AOT text bounds');
  if(ranges.some(r=>lo<r.hi&&r.lo<hi))throw Error('overlapping AOT text');ranges.push({name:m[2],lo,hi});}
 return {ranges,fileOffsetToVA:(off:number)=>{const s=loads.find(s=>off>=s.off&&off<s.off+s.size);return s?s.va+off-s.off:null;}};
}
export function samples(text:string) {
 return text.split('\n').filter(x=>x.trim()).map(line=>{
  const m=line.match(/^\s*(\d+)\.(\d{1,9}):\s+(\d+)\s+([0-9a-f]+)\s+(.+)\s+\((.*)\)$/);
  if(!m)throw Error('unrecognised perf sample: '+line);
  const d=m[6].match(/^(.*)\+0x([0-9a-f]+)$/),ns=BigInt(m[1])*1000000000n+BigInt(m[2].padEnd(9,'0'));
  const period=Number(m[3]);if(!Number.isSafeInteger(period))throw Error('unsafe sample period');
  return{ns,period,symbol:m[5].trim(),dso:d?.[1]??m[6],off:d?parseInt(d[2],16):null};
 });
}
export function phaseAt(ns:bigint,phases:Phase[],lo=0n,hi=0n) {
 for(const p of phases){
  if(ns>=BigInt(p.startNs)+hi&&ns<BigInt(p.endNs)+lo)return p.phase;
  if(ns>=BigInt(p.startNs)+lo&&ns<BigInt(p.endNs)+hi)return 'boundary-uncertain';
 }
 return ns<BigInt(phases[0].startNs)+lo?'startup':'outside-work';
}
export function clockOffset(rows:any[]) {
 if(rows.length!==4)throw Error('missing profile clock anchors');
 let lo:bigint|undefined,hi:bigint|undefined;
 for(const r of rows){const lower=BigInt(r.hostNs)-BigInt(r.afterNs),upper=BigInt(r.hostNs)-BigInt(r.beforeNs);
  if(lo===undefined||lower>lo)lo=lower;if(hi===undefined||upper<hi)hi=upper;}
 if(lo!>hi!)throw Error('inconsistent clock anchors / incompatible constant offset');return{lo:lo!,hi:hi!};
}
export function snapshots(stderr:string) {
 const rows=stderr.split('\n').filter(l=>l.startsWith('AOT_PROFILE_SNAPSHOT ')).map(l=>JSON.parse(l.slice(21)));
 if(!rows.length)return [];
 const names=['warmup.before','warmup.after','steady-state.before','steady-state.after'];
 if(rows.length!==4)throw Error('expected four counter snapshots');
 let previousAfter=0n,previousHost=0n;
 return rows.map((r,i)=>{
  const m=String(r.state).match(/blocks translated or looked up: (\d+), AOT installs: (\d+) \(moved: (\d+), with pc-relative immediates fixed: (\d+), slot taken elsewhere: (\d+)\), reused: (\d+)/);
  const validNs=(v:unknown)=>typeof v==='string'&&/^\d{1,24}$/.test(v);
  if(r.schema!==1||r.label!==names[i]||!validNs(r.beforeNs)||!validNs(r.afterNs)||!validNs(r.hostNs)||
   BigInt(r.afterNs)<BigInt(r.beforeNs)||BigInt(r.beforeNs)<previousAfter||i>0&&BigInt(r.hostNs)<=previousHost||
   r.state!==null&&(typeof r.state!=='string'||!m||m.slice(1).some(x=>!Number.isSafeInteger(Number(x)))))throw Error('invalid counter snapshot');
  previousAfter=BigInt(r.afterNs);previousHost=BigInt(r.hostNs);
  return{...r,blocks:m?Number(m[1]):null,installs:m?Number(m[2]):null,busy:m?Number(m[5]):null};
 });
}
export function analyse(text:string,stderr:string,binary:string,bytes:Buffer,symbols:string,runtime:string,version:string,count:number,aot:boolean) {
 const layout=elfLayout(bytes,symbols),phases=parsePhases(stderr,runtime,version,count,1),rows=samples(text),counters=snapshots(stderr);
 if(aot&&(!layout.ranges.length||counters.length!==4||counters.some(c=>c.blocks===null)))throw Error('missing AOT ranges/counters');
 for(let i=0;i<phases.length&&counters.length===4;i++)
  if(BigInt(counters[i*2].afterNs)>BigInt(phases[i].startNs)||BigInt(counters[i*2+1].beforeNs)<BigInt(phases[i].endNs))throw Error('snapshots do not bracket phase');
 const clock=clockOffset(counters),where=(ns:bigint)=>phaseAt(ns,phases,clock.lo,clock.hi);
 if(!rows.some(r=>where(r.ns)==='steady-state'))throw Error('no steady samples / incompatible clocks');
 const bins=new Map<string,any>();
 for(const s of rows){const phase=where(s.ns);let kind='other-dso',va:number|null=null;
  if(s.dso===binary&&s.off!==null){va=layout.fileOffsetToVA(s.off);const image=layout.ranges.find(r=>va!==null&&va>=r.lo&&va<r.hi);
   kind=image?'native-image:'+image.name:/^(aot_|ctx_get|reg_key|key_content|slot_claim)/.test(s.symbol)?'lookup-install':/^(gen_|jit_(block|step|units))/.test(s.symbol)?'translation':/^(back_|_?gadget_)/.test(s.symbol)?'gadgets':/^(cpu_run|fiber_)/.test(s.symbol)?'dispatch-boundary':'other-emulator';
  }else if(s.dso==='[unknown]')kind='unresolved';
  const key=phase+'|'+kind;let b=bins.get(key);if(!b)bins.set(key,b={phase,kind,samples:0,cycles:0});b.samples++;b.cycles+=s.period;
 }
 const totals=new Map<string,number>();for(const b of bins.values())totals.set(b.phase,(totals.get(b.phase)??0)+b.cycles);
 return{clock:{lowerOffsetNs:String(clock.lo),upperOffsetNs:String(clock.hi)},ranges:layout.ranges,phases,counters,buckets:[...bins.values()].map(b=>({...b,cycleFraction:b.cycles/(totals.get(b.phase)||1)})),
  nativeHotPCs:rows.filter(s=>s.dso===binary&&s.off!==null&&where(s.ns)!=='boundary-uncertain'&&layout.ranges.some(r=>{const va=layout.fileOffsetToVA(s.off!);return va!==null&&va>=r.lo&&va<r.hi;})).reduce((m,s)=>{const phase=where(s.ns),key=String(s.off);const p=m[phase]??(m[phase]={});p[key]=(p[key]??0)+s.period;return m;},{} as Record<string,Record<string,number>>),
  limits:'User-cycle sampling, not instruction/dispatch coverage. Native ranges include stubs/checkpoints and TLB guards, not just useful work. Clock offsets use intersected read brackets and assume a constant offset over the run; small drift within read uncertainty or between anchors cannot be excluded. Boundary-uncertain samples are excluded from work and hot-PC attribution. Snapshots themselves execute guest code; deltas include instrumentation overhead. Profile runs are not timing/RSS evidence.'};
}
export async function main(args:string[]) {
 const [config,out,work='64']=args,count=Number(work);
 if(args.length>3||!isAbsolute(config??'')||!isAbsolute(out??'')||!Number.isInteger(count)||count<1||count>64)throw Error('phase-profile.ts ABS_CONFIG NEW_ABS_OUT [1..64]');
 const frozen=freeze(JSON.parse(readFileSync(config,'utf8'))),all=[...frozen.all,fileHash(import.meta.path),fileHash('/usr/bin/perf'),fileHash('/usr/bin/nm')];
 mkdirSync(out);const manifest={frozen,all,count,event:'cycles:u',frequency:499,clock:'mono',diagnosticOnly:true};
 writeFileSync(join(out,'manifest.json'),JSON.stringify(manifest,null,2));const hash=digest(JSON.stringify(manifest));
 try{for(const k of frozen.config.cases)for(const v of k.variants){
  verifyFiles(all);const id=k.name+'-'+v.name,argv=v.args.map(a=>a==='{repeats}'?String(count):a==='{warmup}'?'1':a),aot=v.name!=='gadget';argv.push(aot?'--profile-jit':'--profile-clock');
  const data=join(out,id+'.data'),env=cleanEnv(aot?'linked-no-emitter':'gadget');
  const cmd=['/usr/bin/timeout','-k','3',String(frozen.config.timeoutSeconds),'/usr/bin/perf','record','--no-buildid-cache','--clockid','mono','-e','cycles:u','-F','499','-o',data,'--',v.binary,...argv];
  const p=Bun.spawnSync(cmd,{env,stdout:'pipe',stderr:'pipe',timeout:Math.ceil((frozen.config.timeoutSeconds+5)*1000),maxBuffer:16*1024*1024});
  const stdout=p.stdout.toString(),stderr=p.stderr.toString();writeFileSync(join(out,id+'.out'),stdout);writeFileSync(join(out,id+'.err'),stderr);
  if(p.exitCode!==0)throw Error(id+': perf failed');assertOracle(stdout,oracle(count));if(aot)assertDiagnostics('linked-no-emitter',stderr,v.imageIndexes.length);
  const q=Bun.spawnSync(['/usr/bin/perf','script','-i',data,'--ns','-F','time,period,ip,sym,dso,dsoff'],{stdout:'pipe',stderr:'pipe',timeout:60000,maxBuffer:64*1024*1024});
  writeFileSync(join(out,id+'.samples'),q.stdout);writeFileSync(join(out,id+'.script.err'),q.stderr);if(q.exitCode!==0)throw Error('perf script failed');
  const nm=Bun.spawnSync(['/usr/bin/nm','-n',v.binary],{stdout:'pipe',stderr:'pipe',timeout:60000});if(nm.exitCode!==0)throw Error('nm failed');
  const summary=analyse(q.stdout.toString(),stderr,realpathSync(v.binary),readFileSync(v.binary),nm.stdout.toString(),k.runtime,k.version,count,aot);
  writeFileSync(join(out,id+'.json'),JSON.stringify({id,manifestSha256:hash,command:cmd,...summary},null,2));verifyFiles(all);console.log('phase-profile-ok '+id);
 }writeFileSync(join(out,'PASS'),hash+'\n');}catch(e){writeFileSync(join(out,'FAIL'),String(e)+'\n');throw e;}
}
if(import.meta.main)await main(process.argv.slice(2));

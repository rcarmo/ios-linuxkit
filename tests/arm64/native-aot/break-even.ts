#!/usr/bin/env bun
/** Explicit-artifact, Linux-only startup/steady-state replay; never trains/builds/stages. */
import {readFileSync,mkdirSync,writeFileSync,appendFileSync,chmodSync,realpathSync} from 'node:fs';
import {isAbsolute,join} from 'node:path';
import {ABI,CODE_VERSION,assertContract,assertOracle,assertDiagnostics,cleanEnv,digest,fileHash,verifyFiles} from './heldout';
export {ABI,CODE_VERSION};
type Identity = ReturnType<typeof fileHash>;
export type Module = {file:string; image:string; recording:string; abi:string; codeVersion:number;
 moduleSha256:string; imageSha256:string; recordingSha256:string};
export type Variant = {name:'gadget'|'full-aot'|'selected-aot'; binary:string; buildOptions:string;
 args:string[]; versionArgs:string[]; versionOracle:string; imageIndexes:number[]};
export type Config = {abi:string; codeVersion:number; trainingFiles:string[]; evaluationFiles:string[];
 metadataFiles:string[]; records:string; modules:Module[]; counts?:number[]; warmup?:number;
 runs:number; timeoutSeconds:number; totalSeconds:number;
 cases:{name:string; runtime:'python'|'bun'; version:string; workload:string; variants:Variant[]}[]};
export const COUNTS = [1,8,32];
export const CORPUS_SHA256 = 'ebf040e9aff5c2b85741e9348bef69f75aeb1d163a7810d4e74ce7a9de149f3e';
export function oracle(count:number):string {
 return `BREAK_EVEN_JSON 64 42 ${count} ${12515468*count} ${CORPUS_SHA256}\n`;
}
function bounded(n:number,min:number,max:number,integer=false):boolean {
 return Number.isFinite(n) && n>=min && n<=max && (!integer || Number.isInteger(n));
}
export function validateConfig(c:Config) {
 assertContract(c.abi,c.codeVersion);
 c.counts ??= [...COUNTS]; c.warmup ??= 1;
 if (!Array.isArray(c.counts) || !c.counts.length || c.counts.length>8 || new Set(c.counts).size!==c.counts.length ||
  c.counts.some(n=>!bounded(n,1,64,true)) || !bounded(c.warmup,0,8,true) || !bounded(c.runs,1,10,true) ||
  !bounded(c.timeoutSeconds,0.05,60) || !bounded(c.totalSeconds,1,1800)) throw Error('unbounded configuration');
 if (!c.trainingFiles?.length || !c.evaluationFiles?.length || !Array.isArray(c.metadataFiles) ||
  !c.modules?.length || c.modules.length>128 || !c.cases?.length || c.cases.length>8) throw Error('explicit files/cases required');
 const names = new Set<string>();
 for (const k of c.cases) {
  if (!/^[a-z0-9-]+$/.test(k.name) || names.has(k.name) || !['python','bun'].includes(k.runtime) ||
   !/^\d+\.\d+\.\d+(?:[-+][\w.-]+)?$/.test(k.version) || k.variants.length!==3) throw Error('invalid case/version');
  names.add(k.name);
  for (const name of ['gadget','full-aot','selected-aot']) {
   const v=k.variants.find(v=>v.name===name);
   if (!v || !Array.isArray(v.args) || !Array.isArray(v.versionArgs) || !v.versionArgs.length ||
    [...v.args,...v.versionArgs].some(a=>typeof a!=='string') ||
    !v.args.includes('{repeats}') || !v.args.includes('{warmup}') ||
    !v.versionOracle || !v.versionOracle.endsWith('\n') || !Array.isArray(v.imageIndexes) ||
    new Set(v.imageIndexes).size!==v.imageIndexes.length || v.imageIndexes.some(i=>!bounded(i,0,c.modules.length-1,true)))
    throw Error('invalid variant');
   if (name==='gadget' && v.imageIndexes.length || name!=='gadget' && !v.imageIndexes.length) throw Error('invalid image selection');
  }
  const full=k.variants.find(v=>v.name==='full-aot')!.imageIndexes.map(i=>c.modules[i]);
  const selected=k.variants.find(v=>v.name==='selected-aot')!.imageIndexes.map(i=>c.modules[i]);
  // Selection can reduce translations within every module, not just omit whole
  // images. Full and selected assemblies are separate frozen identities.
  if(selected.some(m=>!full.some(f=>f.moduleSha256===m.moduleSha256)) ||
   JSON.stringify(selected.map(m=>m.imageSha256).sort())===JSON.stringify(full.map(m=>m.imageSha256).sort())) throw Error('invalid image selection');
 }
}
export function freeze(input:Config) {
 const config:Config=JSON.parse(JSON.stringify(input)); validateConfig(config);
 const trainingFiles=config.trainingFiles.map(fileHash), evaluationFiles=config.evaluationFiles.map(fileHash);
 if (evaluationFiles.some(e=>trainingFiles.some(t=>e.path===t.path || e.sha256===t.sha256))) throw Error('training/evaluation overlap');
 const records=fileHash(config.records);
 if (!evaluationFiles.some(e=>e.path===records.path)) throw Error('records must be frozen evaluation input');
 const rows=JSON.parse(readFileSync(records.path,'utf8'));
 const expected=Array.from({length:64},(_,i)=>({id:i,group:i%17,active:i%3!==0,value:(101+7919*i)%65521,label:'record-'+i.toString(16)}));
 if (!Array.isArray(rows) || rows.length<64 || expected.some((e,i)=>!rows[i] || Object.keys(rows[i]).length!==5 ||
  Object.entries(e).some(([k,v])=>rows[i][k]!==v))) throw Error('fixed 64-record corpus mismatch');
 const metadataFiles=config.metadataFiles.map(fileHash), modules:Identity[]=[], images:Identity[]=[], recordings:Identity[]=[];
 for (const m of config.modules) {
  assertContract(m.abi,m.codeVersion);
  const module=fileHash(m.file), image=fileHash(m.image), recording=fileHash(m.recording);
  if (module.sha256!==m.moduleSha256 || image.sha256!==m.imageSha256 || recording.sha256!==m.recordingSha256)
   throw Error('module/image/recording hash mismatch');
  if (!readFileSync(image.path,'utf8').includes(String(parseInt(ABI,16)))) throw Error('assembly ABI mismatch');
  if (!trainingFiles.some(t=>t.path===recording.path)) throw Error('recording must be frozen training file');
  modules.push(module); images.push(image); recordings.push(recording);
 }
 if (new Set(images.map(i=>i.path)).size!==images.length) throw Error('duplicate images');
 const binaries:Identity[]=[],builds:Identity[]=[],linkedAliases:Identity[]=[];
 for (const k of config.cases) {
  const workload=fileHash(k.workload);
  if (!evaluationFiles.some(e=>e.path===workload.path)) throw Error('workload must be frozen evaluation file');
  for (const v of k.variants) {
   const binary=fileHash(v.binary), build=fileHash(v.buildOptions); binaries.push(binary); builds.push(build); v.binary=binary.path;
   const options=JSON.parse(readFileSync(build.path,'utf8'));
   if (!Array.isArray(options)) throw Error('Meson buildoptions array required');
   const value=(key:string)=>options.find((o:any)=>o.name===key)?.value;
   if (value('guest_arch')!=='arm64' || value('jit')!==(v.name!=='gadget') || (v.name!=='gadget' && value('jit_emit')!==false) || !Array.isArray(value('cli_aot')))
    throw Error('build contract mismatch (arm64, jit, jit_emit, cli_aot)');
   const linked=value('cli_aot').map((p:string)=>fileHash(p)); linkedAliases.push(...linked);
   const actual=linked.map((f:Identity)=>f.path).sort();
   const wanted=v.imageIndexes.map(i=>images[i].path).sort();
   if (JSON.stringify(actual)!==JSON.stringify(wanted)) throw Error('linked build image mismatch');
  }
 }
 const tools=[fileHash('/usr/bin/time'),fileHash('/usr/bin/timeout')];
 const harness=[fileHash(import.meta.path),fileHash(join(import.meta.dir,'heldout.ts'))];
 const all=[...trainingFiles,...evaluationFiles,...metadataFiles,...modules,...images,...recordings,...binaries,...builds,...linkedAliases,...tools,...harness];
 const aliases=new Map(all.map(f=>[f.requestedPath,f.path]));
 for (const k of config.cases) for (const v of k.variants) {
  v.args=v.args.map(a=>aliases.get(a)??a); v.versionArgs=v.versionArgs.map(a=>aliases.get(a)??a);
 }
 // Opaque training/build manifests are hashed, never guessed at or used to select/train.
 return JSON.parse(JSON.stringify({schema:1,abi:ABI,codeVersion:CODE_VERSION,config,all,
  trainingFiles,evaluationFiles,metadataFiles,modules,images,binaries,builds,tools,harness,
  corpus:{source:records,selection:'first 64 records, fixed ids 0..63',sha256:CORPUS_SHA256},
  method:'fresh processes; paired gadget/full and gadget/selected alternating order; warmup retained; no load-time, cold-disk or disk-warmup claims; no causal startup decomposition'}));
}
export type Phase={schema:number;runtime:string;version:string;clock:string;phase:string;workCount:number;startNs:string;endNs:string;seconds:number};
export function parsePhases(stderr:string,runtime:string,version:string,repeats:number,warmup:number):Phase[] {
 const lines=stderr.split('\n').filter(l=>l.startsWith('BREAK_EVEN_PHASE'));
 if (lines.length!==2) throw Error('expected exactly two phase records');
 let previous=0n;
 return lines.map((line,i)=>{
  if (!line.startsWith('BREAK_EVEN_PHASE ')) throw Error('malformed phase prefix');
  const p:Phase=JSON.parse(line.slice('BREAK_EVEN_PHASE '.length));
  if (p.schema!==1 || p.runtime!==runtime || p.version!==version || p.clock!=='monotonic' ||
   p.phase!==['warmup','steady-state'][i] || p.workCount!==[warmup,repeats][i] ||
   typeof p.startNs!=='string' || typeof p.endNs!=='string' || !/^\d{1,24}$/.test(p.startNs) || !/^\d{1,24}$/.test(p.endNs) ||
   !bounded(p.seconds,0,3600)) throw Error('malformed phase metadata');
  const start=BigInt(p.startNs),end=BigInt(p.endNs);
  if (end<start || start<previous || Math.abs(p.seconds-Number(end-start)/1e9)>1e-9) throw Error('malformed phase duration/order');
  previous=end; return p;
 });
}
export function separation(wallSeconds:number,phases:Phase[]) {
 const reportedSeconds=phases.reduce((s,p)=>s+p.seconds,0);
 const residualSeconds=wallSeconds-reportedSeconds;
 return {reportedSeconds,residualSeconds,startupEstimateSeconds:Math.max(0,residualSeconds),
  floored:residualSeconds<0,model:'host GNU wall minus guest/runtime monotonic phases; residual includes input/import/telemetry/output/runner overhead and clock/accounting precision, not causal startup'};
}
export function accounting(text:string) {
 const tokens=text.trim().split('\n').at(-1)?.trim().split(/\s+/)??[];
 const n=tokens.map(Number);
 if (n.length!==7 || n.some(x=>!Number.isFinite(x)||x<0) || n.slice(3).some(x=>!Number.isInteger(x))) throw Error('invalid GNU time accounting');
 const [wallSeconds,userSeconds,systemSeconds,maxRssKiB,minorFaults,majorFaults,exitStatus]=n;
 return {wallSeconds,userSeconds,systemSeconds,maxRssKiB,minorFaults,majorFaults,exitStatus};
}
export function breakEven(samples:{variant:string;count:number;wallSeconds:number}[],opponent:string) {
 function fit(variant:string) {
  const rows=samples.filter(r=>r.variant===variant);
  if (new Set(rows.map(r=>r.count)).size<2) return null;
  const x=rows.reduce((s,r)=>s+r.count,0)/rows.length,y=rows.reduce((s,r)=>s+r.wallSeconds,0)/rows.length;
  const slope=rows.reduce((s,r)=>s+(r.count-x)*(r.wallSeconds-y),0)/rows.reduce((s,r)=>s+(r.count-x)**2,0);
  const intercept=y-slope*x;
  const rmse=Math.sqrt(rows.reduce((s,r)=>s+(r.wallSeconds-intercept-slope*r.count)**2,0)/rows.length);
  return {interceptSeconds:intercept,secondsPerWork:slope,rmseSeconds:rmse,samples:rows.length};
 }
 const gadget=fit('gadget'),aot=fit(opponent);
 const saving=gadget && aot ? gadget.secondsPerWork-aot.secondsPerWork : null;
 const crossing=gadget && aot && saving!==null && saving>0 && gadget.secondsPerWork>=0 && aot.secondsPerWork>=0
  ? (aot.interceptSeconds-gadget.interceptSeconds)/saving : null;
 const range=samples.length?[Math.min(...samples.map(r=>r.count)),Math.max(...samples.map(r=>r.count))]:null;
 return {gadget,aot,crossingWorkCount:crossing,nonnegativeCrossingWorkCount:crossing===null?null:Math.max(0,crossing),
  measuredRange:range,withinMeasuredRange:crossing!==null && range!==null && crossing>=range[0] && crossing<=range[1],
  model:'descriptive OLS host total wall = intercept + slope * work; no causal startup, confidence or extrapolated performance claim; null if unidentified/non-saving/negative slope'};
}
export async function run(configPath:string,output:string) {
 if (process.platform!=='linux' || !isAbsolute(output)) throw Error('Linux and absolute new output required');
 const started=performance.now();
 const inputFile=fileHash(configPath),manifest=freeze(JSON.parse(readFileSync(inputFile.path,'utf8')));
 manifest.all.push(inputFile);
 const deadline=started+manifest.config.totalSeconds*1000;
 mkdirSync(output); // Exclusive creation; never touch rootfs or an existing evidence directory.
 const manifestPath=join(output,'manifest.json'),bytes=JSON.stringify(manifest,null,2)+'\n',hash=digest(bytes);
 writeFileSync(manifestPath,bytes); chmodSync(manifestPath,0o444); writeFileSync(join(output,'manifest.sha256'),hash+'\n');
 function check() {
  if (performance.now()>=deadline) throw Error('total time budget exhausted');
  verifyFiles(manifest.all);
  if (digest(readFileSync(manifestPath))!==hash) throw Error('frozen manifest mutated');
 }
 function execute(id:string,v:Variant,args:string[],detail:Record<string,unknown>) {
  check();
  // Reserve grace inside the total deadline. GNU timeout (no --foreground) owns the child group.
  const remaining=(deadline-performance.now())/1000;
  const seconds=Math.min(manifest.config.timeoutSeconds,remaining-3);
  if (seconds<0.05) throw Error('total time budget exhausted before command');
  const timePath=join(output,id+'.time');
  const command=[manifest.tools[1].path,'-k','1','-s','TERM',seconds.toFixed(3)+'s',manifest.tools[0].path,
   '-f','%e %U %S %M %R %F %x','-o',timePath,v.binary,...args];
  const env=cleanEnv(v.name==='gadget'?'gadget':'linked-no-emitter');
  const before=performance.now();
  const child=Bun.spawnSync(command,{env,stdout:'pipe',stderr:'pipe',timeout:Math.ceil((seconds+2)*1000),killSignal:'SIGKILL',maxBuffer:4*1024*1024});
  const hostCommandSeconds=(performance.now()-before)/1000;
  const stdout=child.stdout.toString(),stderr=child.stderr.toString();
  writeFileSync(join(output,id+'.stdout'),stdout); writeFileSync(join(output,id+'.stderr'),stderr);
  let metricsText='',metrics:ReturnType<typeof accounting>|null=null,error:string|null=null;
  try {metricsText=readFileSync(timePath,'utf8');metrics=accounting(metricsText);} catch(e) {error=String(e);}
  const row={id,...detail,variant:v.name,command,env:Object.fromEntries(Object.entries(env).filter(([k])=>k.startsWith('ISH_'))),
   hostCommandSeconds,processExitCode:child.exitCode,metricsText,metrics,stdout,stderr,error,manifestSha256:hash};
  appendFileSync(join(output,'raw.jsonl'),JSON.stringify(row)+'\n'); // Failed samples remain evidence.
  if (child.exitCode!==0 || !metrics || metrics.exitStatus!==0) throw Error(id+': process/accounting failed: '+error);
  check(); return {row,metrics,stdout,stderr};
 }
 const samples:{case:string;opponent:string;variant:string;count:number;wallSeconds:number}[]=[];
 try {
  // Version checks are separate fresh processes, included in the total budget, not measured samples.
  for (const k of manifest.config.cases) for (const v of k.variants) {
   const r=execute(k.name+'-'+v.name+'-version',v,v.versionArgs,{kind:'version',case:k.name,expectedVersion:k.version});
   assertOracle(r.stdout,v.versionOracle);
  }
  for (const k of manifest.config.cases) for (const count of manifest.config.counts) for (let pair=0;pair<manifest.config.runs;pair++) {
   for (const opponent of ['full-aot','selected-aot']) {
    const order=pair%2?[opponent,'gadget']:['gadget',opponent];
    for (let position=0;position<2;position++) {
     const v=k.variants.find((v:Variant)=>v.name===order[position])!;
     const id=`${k.name}-${count}-${pair}-${opponent}-${position}`;
     const args=v.args.map((a:string)=>a==='{repeats}'?String(count):a==='{warmup}'?String(manifest.config.warmup):a);
     const r=execute(id,v,args,{kind:'measurement',case:k.name,count,pair,opponent,position,runtime:k.runtime,version:k.version});
     assertOracle(r.stdout,oracle(count));
     const phases=parsePhases(r.stderr,k.runtime,k.version,count,manifest.config.warmup);
     assertDiagnostics(v.name==='gadget'?'gadget':'linked-no-emitter',r.stderr,v.imageIndexes.length);
     samples.push({case:k.name,opponent,variant:v.name,count,wallSeconds:r.metrics.wallSeconds});
     appendFileSync(join(output,'phases.jsonl'),JSON.stringify({id,manifestSha256:hash,phases,...separation(r.metrics.wallSeconds,phases)})+'\n');
    }
   }
  }
  check();
  writeFileSync(join(output,'models.json'),JSON.stringify(manifest.config.cases.flatMap((k:any)=>['full-aot','selected-aot'].map(opponent=>
   ({case:k.name,opponent,manifestSha256:hash,...breakEven(samples.filter(s=>s.case===k.name && s.opponent===opponent),opponent)}))),null,2)+'\n');
  writeFileSync(join(output,'PASS'),'exact oracles, phases, versions, diagnostics and frozen identities verified\n');
  console.log('break-even-ok: '+output);
 } catch(e) {writeFileSync(join(output,'FAIL'),String(e)+'\n');throw e;}
}
if (import.meta.main) {
 const [config,out,...extra]=process.argv.slice(2);
 if (!config || !out || extra.length) throw Error('usage: break-even.ts ABS_CONFIG NEW_ABS_OUTPUT');
 await run(config,out);
}

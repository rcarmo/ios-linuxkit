import {test,expect,afterAll} from 'bun:test';
import {mkdtempSync,writeFileSync,rmSync,symlinkSync,unlinkSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {ABI,CODE_VERSION,COUNTS,oracle,parsePhases,validateConfig,freeze,accounting,separation,breakEven,type Config} from './break-even';
import {assertOracle,cleanEnv,digest,fileHash,verifyFiles} from './heldout';
const corpusDir=mkdtempSync(join(tmpdir(),'break-even-corpus-'));
const records=join(corpusDir,'records.json');
writeFileSync(records,JSON.stringify(Array.from({length:64},(_,i)=>({id:i,group:i%17,active:i%3!==0,value:(101+7919*i)%65521,label:'record-'+i.toString(16)}))));
afterAll(()=>rmSync(corpusDir,{recursive:true}));
const python=process.env.BREAK_EVEN_TEST_PYTHON??'/usr/bin/python3',bun=process.env.BREAK_EVEN_TEST_BUN??process.execPath;
function fixture(dir:string):Config {
 const put=(name:string,bytes:string)=>{const p=join(dir,name);writeFileSync(p,bytes);return p;};
 const training=put('training','training only'),workload=join(import.meta.dir,'break-even.py');
 const modules=[0,1].map(i=>{
  const file=put('module'+i,'ELF fixture '+i),image=put('image'+i+'.S',`.long ${parseInt(ABI,16)}\n`),recording=put('recording'+i,'recording '+i);
  return {file,image,recording,abi:ABI,codeVersion:CODE_VERSION,moduleSha256:fileHash(file).sha256,imageSha256:fileHash(image).sha256,recordingSha256:fileHash(recording).sha256};
 });
 const variants=['gadget','full-aot','selected-aot'].map((name,i)=>{
  const imageIndexes=i===0?[]:i===1?[0,1]:[1];
  const opts=[{name:'guest_arch',value:'arm64'},{name:'jit',value:i!==0},{name:'jit_emit',value:i===0},{name:'cli_aot',value:imageIndexes.map(j=>modules[j].image)}];
  return {name,binary:'/bin/true',buildOptions:put(name+'.json',JSON.stringify(opts)),args:['{repeats}','{warmup}'],versionArgs:['--version'],versionOracle:'fixture\n',imageIndexes};
 });
 return {abi:ABI,codeVersion:CODE_VERSION,trainingFiles:[training,...modules.map(m=>m.recording)],evaluationFiles:[records,workload],metadataFiles:[],records,modules,
  runs:1,timeoutSeconds:1,totalSeconds:10,cases:[{name:'python',runtime:'python',version:'3.13.5',workload,variants:variants as any}]};
}
test('native Python/Bun exact common oracle at 1,8,32; phases are not oracles',()=>{
 const versions=[Bun.spawnSync([python,'-c','import platform; print(platform.python_version())']).stdout.toString().trim(),Bun.spawnSync([bun,'--version']).stdout.toString().trim()];
 for (const [index,runtime] of ['python','bun'].entries()) for (const count of COUNTS) {
  const workload=join(import.meta.dir,index===0?'break-even.py':'break-even-bun.ts');
  const r=Bun.spawnSync(['/usr/bin/timeout','-k','1','5s',index===0?python:bun,workload,records,String(count),'1'],
   {env:cleanEnv('host-native'),stdout:'pipe',stderr:'pipe',timeout:7000});
  expect(r.exitCode).toBe(0);assertOracle(r.stdout.toString(),oracle(count));
  const phases=parsePhases(r.stderr.toString(),runtime,versions[index],count,1);
  expect(phases[1].workCount).toBe(count);
 }
 // Guest Bun 1.4.4 is a distinct declared/checked runtime, not the native reference.
 expect(versions[1]).toMatch(/^\d+\.\d+\.\d+/);
 for (const runtime of [python,bun]) {
  const script=join(import.meta.dir,runtime===python?'break-even.py':'break-even-bun.ts');
  expect(Bun.spawnSync([runtime,script,records,'65','1'],{stdout:'pipe',stderr:'pipe',timeout:2000}).exitCode).not.toBe(0);
 }
},15000);
test('phase parser rejects missing, duplicate, malformed, reversed, wrong counts/version and clock',()=>{
 const p={schema:1,runtime:'bun',version:'1.4.4',clock:'monotonic',phase:'warmup',workCount:1,startNs:'10',endNs:'20',seconds:1e-8};
 const q={...p,phase:'steady-state',workCount:8,startNs:'30',endNs:'50',seconds:2e-8};
 const text=(a:any,b:any)=>'BREAK_EVEN_PHASE '+JSON.stringify(a)+'\nBREAK_EVEN_PHASE '+JSON.stringify(b)+'\n';
 expect(parsePhases(text(p,q),'bun','1.4.4',8,1)).toHaveLength(2);
 for (const changes of [{seconds:-1},{seconds:2},{startNs:'60'},{workCount:32},{version:'1.3.10'},{clock:'wall'},{endNs:'x'}])
  expect(()=>parsePhases(text(p,{...q,...changes}),'bun','1.4.4',8,1)).toThrow();
 for (const bad of ['',text(p,p),text(p,q)+text(p,q),'BREAK_EVEN_PHASE junk\n',text(q,p)])
  expect(()=>parsePhases(bad,'bun','1.4.4',8,1)).toThrow();
});
test('config rejects stale ABI/code and all bounds before file access',()=>{
 const d=mkdtempSync(join(tmpdir(),'break-even-config-'));
 try {
  const c=fixture(d);validateConfig(c);expect(c.counts).toEqual(COUNTS);
  for (const changes of [{abi:'old'},{codeVersion:14},{counts:[65]},{counts:[]},{counts:[1,1]},{warmup:9},{runs:11},{timeoutSeconds:61},{totalSeconds:1801},{totalSeconds:NaN}])
   expect(()=>validateConfig({...c,...changes} as Config)).toThrow();
  const bad=structuredClone(c);bad.cases[0].variants[2].imageIndexes=[0,1];expect(()=>validateConfig(bad)).toThrow();
 } finally {rmSync(d,{recursive:true});}
});
test('standalone freeze seals files/metadata, aliases and build selection without interpreting training manifest',()=>{
 const d=mkdtempSync(join(tmpdir(),'break-even-freeze-'));
 try {
  const c=fixture(d),target=c.trainingFiles[0],alias=join(d,'alias');symlinkSync(target,alias);c.trainingFiles[0]=alias;
  c.cases[0].variants[0].args.push(alias);
  const frozen=freeze(c),hash=digest(JSON.stringify(frozen));verifyFiles(frozen.all);
  expect(frozen.config.cases[0].variants[0].args.at(-1)).toBe(target);
  c.modules[0].moduleSha256='changed';expect(digest(JSON.stringify(frozen))).toBe(hash);
  const other=join(d,'same');writeFileSync(other,'training only');unlinkSync(alias);symlinkSync(other,alias);
  expect(()=>verifyFiles(frozen.all)).toThrow('manifest hash mismatch');
  unlinkSync(alias);symlinkSync(target,alias);writeFileSync(target,'new bytes');expect(()=>verifyFiles(frozen.all)).toThrow();
  const fresh=fixture(d);expect(()=>freeze({...fresh,evaluationFiles:[...fresh.evaluationFiles,fresh.trainingFiles[0]]})).toThrow('overlap');
  fresh.modules[0].imageSha256='0'.repeat(64);expect(()=>freeze(fresh)).toThrow('hash mismatch');
  const b=fixture(d);writeFileSync(b.cases[0].variants[1].buildOptions,'[]');expect(()=>freeze(b)).toThrow('build contract');
 } finally {rmSync(d,{recursive:true});}
});
test('GNU timeout kills child group; all inherited ISH variables stripped',()=>{
 const d=mkdtempSync(join(tmpdir(),'break-even-timeout-'));
 try {
  const r=Bun.spawnSync(['/usr/bin/timeout','-k','1','0.05s','/usr/bin/time','-f','%e %U %S %M %R %F %x','-o',join(d,'time'),
   '/bin/sh','-c','(sleep 0.2; echo orphan > "$1") & wait','sh',join(d,'orphan')],{stdout:'pipe',stderr:'pipe'});
  expect(r.exitCode).toBe(124);Bun.sleepSync(250);expect(()=>fileHash(join(d,'orphan'))).toThrow();
  expect(cleanEnv('gadget',{PATH:'/bin',ISH_SOMETHING:'bad',ISH_BLOCK_PROF:'1',ISH_JIT:'1'})).toEqual({PATH:'/bin',ISH_JIT:'0',ISH_AOT_FAMILY:'0',ISH_JIT_STATS:'1'});
 } finally {rmSync(d,{recursive:true});}
});
test('accounting, residual floor and descriptive break-even retain uncertainty',()=>{
 expect(accounting('0.01 0.00 0.00 123 4 0 0\n').maxRssKiB).toBe(123);
 for (const t of ['','1 2','NaN 0 0 0 0 0 0','0 0 0 1.5 0 0 0']) expect(()=>accounting(t)).toThrow();
 const p=[{seconds:0.02}] as any;expect(separation(0.01,p).residualSeconds).toBe(-0.01);expect(separation(0.01,p).startupEstimateSeconds).toBe(0);
 const samples=[1,8,32].flatMap(count=>[{variant:'gadget',count,wallSeconds:1+count*2},{variant:'full-aot',count,wallSeconds:5+count}]);
 expect(breakEven(samples,'full-aot').crossingWorkCount).toBeCloseTo(4,10);
 expect(breakEven(samples.slice(0,2),'full-aot').crossingWorkCount).toBeNull();
 expect(()=>assertOracle(oracle(8).trim(),oracle(8))).toThrow();
});

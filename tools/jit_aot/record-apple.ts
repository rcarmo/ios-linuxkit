#!/usr/bin/env bun
// Train the bundled Apple Bun/Go userland; do not change the Linux Python kit.
import { cpSync, existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { appleBuildVersion } from './apple';
import { checked, definedSymbols, matchContract, seal, sha } from './kit';
import { appleModules } from './apple-modules';

const [recorderArg, rootArg, binaryArg, contractArg, outArg] = process.argv.slice(2);
if (!outArg || process.platform !== 'darwin' || process.arch !== 'arm64')
    throw Error('record-apple.ts RECORDER FAKEFS APP_SYMBOL_BINARY OBSERVED_CONTRACT NEW_OUTPUT (Apple Silicon only)');
const recorder = resolve(recorderArg), root = resolve(rootArg), binary = resolve(binaryArg);
const out = resolve(outArg), stage = out + `.partial-${process.pid}`;
if (existsSync(out)) throw Error('refusing existing output');
const contract = JSON.parse(readFileSync(contractArg, 'utf8'));
const binaryHash = await sha(binary);
const build = appleBuildVersion(readFileSync(binary));
if (build.platform !== contract.platform) throw Error('contract platform mismatch');
matchContract(contract, contract, binaryHash);
const symbols = definedSymbols(checked(['nm', '-g', binary]));
mkdirSync(stage);
const modules = appleModules;
const workload = `set -eu
i=0; while [ "$i" -lt 1000 ]; do i=$((i+1)); done
test "$i" = 1000
/bin/busybox printf 'busybox-ok\\n' | /bin/busybox grep -qx busybox-ok
test "$(/usr/local/bin/bun --version)" = 1.4.2
/usr/local/bin/bun -e 'import {createCipheriv,createDecipheriv} from "node:crypto"; let sum=0; for(let i=0;i<10000;i++)sum+=i; if(JSON.parse(JSON.stringify({sum})).sum!==49995000)throw Error("sum"); const key=Buffer.alloc(16),iv=Buffer.alloc(12); for(let i=0;i<100;i++){const c=createCipheriv("aes-128-gcm",key,iv);const data=Buffer.concat([c.update("aot-test"),c.final()]);const d=createDecipheriv("aes-128-gcm",key,iv);d.setAuthTag(c.getAuthTag());if(Buffer.concat([d.update(data),d.final()]).toString()!=="aot-test")throw Error("crypto");} console.log("BUN_TRAIN_OK")'
printf 'APPLE_AOT_TRAIN_OK\\n'
`;
await Bun.write(join(stage, 'workload.sh'), workload);
cpSync(join(import.meta.dir, '../../tests/arm64/benchmarks/go-aot'), join(stage, 'go-workload'), { recursive: true });
await Bun.write(join(stage, 'target-contract.json'), JSON.stringify(contract, null, 2));
// Record the cold Go build with the compiler first, then reuse only its cache.
// Formatting, direct compile/asm, linking and vet still run on every invocation.
const order = [...modules.slice(0, 3), modules.find(module => module.name === 'go_compile')!,
    ...modules.slice(3).filter(module => module.name !== 'go_compile')];
const existingGoCache = process.env.APPLE_AOT_GO_CACHE;
if (existingGoCache && !/^\/tmp\/go-aot-record-cache-[0-9]+$/.test(existingGoCache))
    throw Error('APPLE_AOT_GO_CACHE must name a previous recording cache in this guest');
const goCache = existingGoCache || `/tmp/go-aot-record-cache-${process.pid}`;
const results: any[] = [];
for (const module of order) {
    const guest = join(root, 'data', module.path), hash = await sha(guest);
    const record = join(stage, module.name + '.jsonl');
    const go = module.name === 'go' || module.name === 'gofmt' || module.name.startsWith('go_');
    console.log(`Recording ${module.name}`);
    const stdout = checked([recorder, '-f', root, '/usr/bin/env',
        'BUN_JSC_useJIT=0', 'BUN_RUNTIME_TRANSPILER_CACHE_PATH=0',
        ...(go ? [`GO_AOT_CACHE=${goCache}`, `GO_AOT_VET_FIRST=${module.name === 'go_vet' ? '1' : '0'}`,
            `GO_AOT_REBUILD=${module.name === 'go_compile' ? '1' : '0'}`] : []),
        '/bin/sh', ...(go ? ['/mnt/go-aot/build.sh'] : ['-ec', workload])], join(stage, module.name + '-record.log'), {
        env: { ISH_JIT: '1', ISH_JIT_PIC: '1', ISH_JIT_STATS: '1',
            ISH_JIT_RECORD: record, ISH_JIT_RECORD_MOD: module.path, ISH_JIT_RECORD_MOD_EXACT: '1', ISH_AOT_FAMILY: '0',
            ...(go ? { ISH_JIT_RECORD_TARGET_ONLY: '1' } : {}),
            ...(go ? { ISH_BIND_MOUNTS: `/mnt/go-aot=${join(stage, 'go-workload')}:ro` } : {}) },
        timeout: go ? 3600000 : 240000,
    });
    if (!stdout.split('\n').includes(go ? 'GO_AOT_BUILD_OK' : 'APPLE_AOT_TRAIN_OK')) throw Error('missing workload completion');
    const rows = readFileSync(record, 'utf8').trim().split('\n').map(line => JSON.parse(line));
    const header = rows.shift()?.header;
    matchContract(header, contract, binaryHash);
    if (go && header.region_exhausted !== false)
        throw Error(`incomplete ${module.name} recording: code arena exhausted or recorder lacks capacity diagnostics`);
    if (!rows.length || rows.some(row => row.header || row.mod !== module.path) || !rows.some(row => row.segs?.length))
        throw Error('invalid/empty recording');
    const required = new Set<string>(['ish_aot_register']);
    for (const row of rows) {
        for (const name of Object.values(row.keysym || {})) required.add(name as string);
        for (const segment of row.segs) for (const rel of segment.rel)
            if (rel[1] !== 'exit') required.add(rel[2]);
    }
    for (const name of required) if (name !== '@region' && !symbols.has(name))
        throw Error(`unresolved target symbol: ${name}`);
    checked(['python3', join(import.meta.dir, 'gen.py'), record, binary, join(root, 'data'),
        join(stage, `aot_${module.name}.S`), '--name', module.name, '--format', 'macho', '--family', ''],
        join(stage, module.name + '-generate.log'));
    if (await sha(guest) !== hash) throw Error('guest changed while recording');
    results.push({ ...module, sha256: hash, translations: rows.length, header });
}
if (!existingGoCache)
    checked([recorder, '-f', root, '/bin/rm', '-rf', goCache], join(stage, 'go-cache-cleanup.log'), { env: { ISH_JIT: '0' } });
results.sort((a, b) => modules.findIndex(module => module.name === a.name) - modules.findIndex(module => module.name === b.name));
await seal(stage, out, { kind: 'apple-bun-images', format: 'macho', contract, modules: results,
    recorderSha256: await sha(recorder), sourceRevision: checked(['git', 'rev-parse', 'HEAD']).trim(),
    reusedGoBuildCache: !!existingGoCache,
    validation: 'recorded/generation only; linked execution and device gates required' });
console.log(`Sealed Apple images: ${out}`);

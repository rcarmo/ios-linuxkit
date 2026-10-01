#!/usr/bin/env bun
/** Frozen evaluation inputs; alternating AOT/off of one binary; wait4 RSS. */
import {join, resolve} from 'node:path';
import {mkdirSync} from 'node:fs';
import {cleanEnv} from '../../../tools/jit_aot/kit';

const [binaryArg, rootArg, outArg] = process.argv.slice(2);
if (!outArg) throw Error('heldout.ts BINARY ROOT OUTPUT');
const binary = resolve(binaryArg), root = resolve(rootArg), out = resolve(outArg);
mkdirSync(out); // A new evidence directory, never overwrite previous samples.
const inputs = join(out, 'inputs'); mkdirSync(inputs);
const cpu = process.env.PERF_CPU || '0';
const runs = Number(process.env.HELDOUT_RUNS || '5');
if (!Number.isInteger(runs) || runs < 1 || runs > 15) throw Error('invalid runs');
const sha = async (p: string) => new Bun.CryptoHasher('sha256').update(await Bun.file(p).arrayBuffer()).digest('hex');
const invoke = (args: string[], env = cleanEnv()) => {
    const p = Bun.spawnSync(args, {env, stdout: 'pipe', stderr: 'pipe', timeout: 120000});
    if (p.exitCode) throw Error(`${args[0]} exit ${p.exitCode}: ${p.stderr.toString().slice(-1000)}`);
    return p;
};
const py = join(inputs, 'heldout.py'), search = join(inputs, 'search.sh');
await Bun.write(py, await Bun.file(join(import.meta.dir, 'heldout.py')).text());
await Bun.write(search, await Bun.file(join(import.meta.dir, 'heldout-search.sh')).text());
const records = Array.from({length: 3000}, (_, i) => ({id: i, group: i % 17,
    active: i % 3 !== 0, value: (i * 7919 + 101) % 65521, label: `record-${i.toString(16)}`}));
await Bun.write(join(inputs, 'records.json'), JSON.stringify(records) + '\n');
const lines = Array.from({length: 18000}, (_, i) => `id=${i} group=${i % 37} tag=${i % 7 === 0 ? 'needle' : 'hay'} value=${(i * 3571) % 99991}`);
await Bun.write(join(inputs, 'records.txt'), lines.join('\n') + '\n');
const payload = new Uint8Array(512 * 1024); let state = 0x6d2b79f5;
for (let i = 0; i < payload.length; i++) {
    state ^= state << 13; state ^= state >>> 17; state ^= state << 5;
    payload[i] = i % 5 ? ((i >>> 3) % 251) : state & 255;
}
await Bun.write(join(inputs, 'payload.bin'), payload);
const hashes: Record<string, string> = {};
for (const name of ['heldout.py', 'search.sh', 'records.json', 'records.txt', 'payload.bin']) hashes[name] = await sha(join(inputs, name));
const guest = '/tmp/aot-heldout';
// Stream through guest tar so fakefs metadata records every evaluation file.
const archive = join(out, 'inputs.tar'); invoke(['tar', '-C', inputs, '-cf', archive, '.']);
const staged = Bun.spawnSync([binary, '-f', root, '/bin/sh', '-ec', `rm -rf ${guest}; mkdir ${guest}; tar -xf - -C ${guest}`],
    {env: cleanEnv(), stdin: await Bun.file(archive).arrayBuffer(), stdout: 'pipe', stderr: 'pipe', timeout: 120000});
await Bun.write(join(out, 'stage.log'), staged.stdout.toString() + staged.stderr.toString());
if (staged.exitCode) throw Error('guest staging failed');
const expected: Record<string, string> = {};
for (const name of ['json', 'codec']) expected[name] = invoke(['python3', py, name, inputs]).stdout.toString().trim();
const searchOutput = invoke([binary, '-f', root, '/bin/busybox', 'sh', `${guest}/search.sh`, guest], cleanEnv({ISH_JIT: '0', ISH_AOT_FAMILY: '0'})).stdout.toString().trim();
// Independent data oracle: selection cardinality plus POSIX cksum of raw text.
const nativeChecksum = invoke(['cksum', join(inputs, 'records.txt')]).stdout.toString().trim().split(/\s+/).slice(0, 2).join(' ');
if (searchOutput.split('\n').filter(x => x === '2572').length !== 3 ||
    searchOutput.split('\n').filter(x => x === nativeChecksum).length !== 3 || !searchOutput.endsWith('HELDOUT_SEARCH_OK')) throw Error('search oracle mismatch');
const nativeSearch = invoke(['/bin/sh', search, inputs]).stdout.toString().trim();
if (searchOutput !== nativeSearch) throw Error('guest search differs from native POSIX tools');
expected.search = nativeSearch;
const launcher = join(out, 'resource');
invoke(['clang', '-O2', '-Wall', '-Wextra', '-Werror', join(import.meta.dir, 'resource.c'), '-o', launcher]);
const commands: Record<string, string[]> = {
    json: ['/usr/bin/python3', `${guest}/heldout.py`, 'json', guest],
    codec: ['/usr/bin/python3', `${guest}/heldout.py`, 'codec', guest],
    search: ['/bin/sh', `${guest}/search.sh`, guest],
};
const result: any = {binary, binarySha256: await sha(binary), root, cpu, runs, hashes, expected,
    training: {manifest: '/workspace/artifacts/ish-aot/2.3.0-prototype/seed/recordings/manifest.json',
        manifestSha256: await sha('/workspace/artifacts/ish-aot/2.3.0-prototype/seed/recordings/manifest.json'),
        workloadSha256: await sha('/workspace/artifacts/ish-aot/2.3.0-prototype/seed/recordings/workload.sh')},
    method: 'Same no-emitter binary AOT enabled/runtime off; warm page cache; fresh processes; alternating paired order; one discarded warm-up per mode/case; wall includes startup and launcher; schedutil unchanged',
    rss: 'wait4 peak bytes for CLI process/descendants, not sampled live RSS, guest-only memory or Apple footprint', cases: {}};
await Bun.write(join(out, 'inputs.json'), JSON.stringify(result, null, 2) + '\n');
const median = (xs: number[]) => [...xs].sort((a,b) => a-b)[Math.floor(xs.length / 2)];
for (const [name, args] of Object.entries(commands)) {
    const rows: any = {off: [], aot: []};
    for (let i = -1; i < runs; i++) for (const mode of i % 2 ? ['aot', 'off'] : ['off', 'aot']) {
        const env = cleanEnv({ISH_JIT: mode === 'aot' ? '1' : '0', ISH_JIT_STATS: '1', ISH_AOT_FAMILY: '0'});
        const start = performance.now();
        const p = invoke([launcher, 'taskset', '-c', cpu, binary, '-f', root, ...args], env);
        const wallMs = performance.now() - start;
        const stdout = p.stdout.toString().trim(), stderr = p.stderr.toString();
        await Bun.write(join(out, `${name}-${mode}-${i}.log`), stdout + '\n' + stderr);
        if (stdout !== expected[name]) throw Error(`${name}/${mode} output differs`);
        if (mode === 'aot' && (!/AOT [1-9][0-9]* \(4 images\)/.test(stderr) || !/segments 0, units 0, code 0 KB/.test(stderr))) throw Error(`${name}: missing AOT/no-emitter proof`);
        const resource = JSON.parse(stderr.match(/^RESOURCE (.+)$/m)?.[1] || 'null');
        if (!resource) throw Error('missing wait4 measurement');
        if (i >= 0) rows[mode].push({pair: i, position: mode === (i % 2 ? 'aot' : 'off') ? 0 : 1, wallMs, ...resource});
    }
    const summary: any = {};
    for (const mode of ['off', 'aot']) summary[mode] = {wallMs: median(rows[mode].map((x:any) => x.wallMs)), maxRSS: median(rows[mode].map((x:any) => x.maxRSS)), cpuUs: median(rows[mode].map((x:any) => x.cpuUs))};
    result.cases[name] = {samples: rows, median: summary, speedup: summary.off.wallMs / summary.aot.wallMs, rssRatio: summary.aot.maxRSS / summary.off.maxRSS};
    await Bun.write(join(out, 'results.json'), JSON.stringify(result, null, 2) + '\n');
    console.log(`${name}: speedup=${result.cases[name].speedup.toFixed(3)} rssRatio=${result.cases[name].rssRatio.toFixed(3)}`);
}
for (const [name, hash] of Object.entries(hashes)) if (await sha(join(inputs, name)) !== hash) throw Error(`mutated input ${name}`);
console.log(`heldout-aot-gate-ok: ${out}`);

/**
 * @summary Paired, CPU-pinned guest poke benchmark with output/resource checks.
 * @usage bun tests/arm64/perf-poke.ts BEFORE AFTER ROOTFS OUT.json [samples=11]
 * Each sample starts a fresh interpreter; two unmeasured warmups per variant
 * keep host page caches warm. No global cache drop or CPU policy change.
 */
import { readFileSync, writeFileSync } from 'node:fs';
const [before, after, rootfs, output, countArg = '11'] = process.argv.slice(2);
if (!before || !after || !rootfs || !output) throw new Error('BEFORE AFTER ROOTFS OUT.json [samples] required');
const count = Number(countArg);
if (!Number.isInteger(count) || count < 3 || count > 101) throw new Error('samples must be 3..101');
const cpu = process.env.PERF_CPU || '11';
if (!/^\d+$/.test(cpu)) throw new Error('invalid PERF_CPU');
if (Object.keys(process.env).some(k => k.startsWith('ISH_'))) throw new Error('remove ISH_* diagnostics/overrides before timing');
const workloads = [
    {name: 'shell-start', args: ['/bin/sh', '-c', 'printf "shell-ok\\n"'], expected: 'shell-ok\n'},
    {name: 'python-start', args: ['/usr/bin/python3', '-S', '-c', 'print("python-ok")'], expected: 'python-ok\n'},
    {name: 'python-compute', args: ['/usr/bin/python3', '-S', '-c', 'print(sum(range(5000000)))'], expected: '12499997500000\n'},
    {name: 'python-io', args: ['/usr/bin/python3', '-S', '-c', 'import tempfile; f=tempfile.TemporaryFile(); b=b"x"*4096; [f.write(b) for _ in range(1024)]; f.seek(0); print(len(f.read()))'], expected: '4194304\n'},
];
const read = (path: string) => { try { return readFileSync(path, 'utf8').trim(); } catch { return null; } };
const hash = async (path: string) => new Bun.CryptoHasher('sha256').update(await Bun.file(path).arrayBuffer()).digest('hex');
const rows: any[] = [];
const report = {date: new Date().toISOString(), before, after, rootfs, cpu, count,
    hashes: {before: await hash(before), after: await hash(after)},
    treatment: 'fresh interpreter every sample; warm host page caches; two discarded warmups; alternating pair order; no instrumentation',
    governor: read(`/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor`),
    min_khz: read(`/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_min_freq`),
    max_khz: read(`/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_max_freq`), workloads, rows};
async function run(w: typeof workloads[number], variant: string, binary: string, sample: number) {
    const start_khz = read(`/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq`);
    const start = performance.now();
    const p = Bun.spawn(['timeout', '-k', '5', '60', 'taskset', '-c', cpu, binary, '-f', rootfs, ...w.args], {stdout: 'pipe', stderr: 'pipe'});
    const [status, stdout, stderr] = await Promise.all([p.exited, new Response(p.stdout).text(), new Response(p.stderr).text()]);
    const wall_ms = performance.now() - start;
    const u = p.resourceUsage()!;
    if (status || stdout !== w.expected || stderr) throw new Error(`${w.name} ${variant}: status=${status} stdout=${JSON.stringify(stdout)} stderr=${stderr}`);
    return {workload: w.name, variant, sample, wall_ms, user_ms: Number(u.cpuTime.user)/1000,
        sys_ms: Number(u.cpuTime.system)/1000, maxRSS_bytes: u.maxRSS, start_khz,
        end_khz: read(`/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq`),
        temp_mC: read('/sys/class/thermal/thermal_zone0/temp'), loadavg: read('/proc/loadavg')};
}
const selected = process.env.PERF_WORKLOAD ? workloads.filter(w => w.name === process.env.PERF_WORKLOAD) : workloads;
if (!selected.length) throw new Error('unknown PERF_WORKLOAD');
for (const w of selected) {
    for (let i=0; i<2; i++) { await run(w,'before',before,-1); await run(w,'after',after,-1); }
    for (let i=0; i<count; i++) {
        const pair = i%2 ? [['after',after],['before',before]] : [['before',before],['after',after]];
        for (const [variant, binary] of pair) rows.push(await run(w,variant,binary,i));
        writeFileSync(output, JSON.stringify(report,null,2)+'\n');
    }
    for (const variant of ['before','after']) {
        const r=rows.filter(r=>r.workload===w.name && r.variant===variant);
        const a=r.map(r=>r.wall_ms).sort((a,b)=>a-b);
        const q=(p:number)=>a[Math.round((a.length-1)*p)];
        console.log(`${w.name} ${variant} n=${a.length} wall_ms p10=${q(.1).toFixed(2)} median=${q(.5).toFixed(2)} p90=${q(.9).toFixed(2)} user_ms=${r.map(r=>r.user_ms).sort((a,b)=>a-b)[Math.floor(r.length/2)].toFixed(2)}`);
    }
}

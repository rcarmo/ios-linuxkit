#!/usr/bin/env bun
// Credentials are exposed only for this subprocess, never saved in the guest.
import { chmodSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { homedir, tmpdir } from 'node:os';

const [ishArg, rootArg, mode = 'offline', runsArg = mode === 'copilot' ? '1' : '3'] = process.argv.slice(2);
const runs = Number(runsArg);
if (!ishArg || !rootArg || !['offline', 'copilot'].includes(mode) || !Number.isInteger(runs) || runs < 1 || runs > 10)
    throw Error('Usage: benchmark-pi-guest.ts ISH FAKEFS [offline|copilot] [RUNS 1..10]');
const ish = resolve(ishArg), root = resolve(rootArg);
const fixtures = resolve(import.meta.dir, '../tests/arm64/benchmarks');
const stage = mkdtempSync(join(tmpdir(), 'linuxkit-pi-auth-'));
chmodSync(stage, 0o700);
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
    !/API_KEY|TOKEN|CREDENTIAL|^ISH_JIT|^ISH_AOT|^ISH_BIND_MOUNTS/.test(key)));
try {
    if (mode === 'copilot') {
        const path = process.env.PI_AUTH_FILE || join(homedir(), '.pi/agent/auth.json');
        let credential = JSON.parse(readFileSync(path, 'utf8'))['github-copilot'];
        if (credential?.type !== 'oauth') throw Error('No local GitHub Copilot OAuth credential');
        const { githubCopilotOAuth } = await import(join(root, 'data/opt/pi/node_modules/@earendil-works/pi-ai/dist/auth/oauth/github-copilot.js'));
        let auth;
        try {
            if (credential.expires <= Date.now() + 240000) {
                credential = await githubCopilotOAuth.refresh(credential, AbortSignal.timeout(30000));
            }
            auth = await githubCopilotOAuth.toAuth(credential);
        } catch {
            throw Error('Copilot credential refresh failed; reauthenticate locally before retrying');
        }
        writeFileSync(join(stage, 'request.json'), JSON.stringify(auth), { mode: 0o600 });
    }
    const mkdir = Bun.spawnSync([ish, '-f', root, '/bin/mkdir', '-p', '/mnt/benchmark', '/mnt/pi-auth'],
        { env, stdout: 'pipe', stderr: 'pipe', timeout: 10000 });
    if (mkdir.exitCode) throw Error('Guest mountpoint creation failed');
    env.ISH_BIND_MOUNTS = `/mnt/benchmark=${fixtures}:ro,/mnt/pi-auth=${stage}:ro`;
    for (let run = 1; run <= runs; run++) {
        const start = performance.now();
        const child = Bun.spawn([ish, '-f', root, '/usr/bin/env', 'BUN_JSC_useJIT=0',
            'BUN_JSC_numberOfGCMarkers=1', 'BUN_JSC_useConcurrentGC=0', 'DO_NOT_TRACK=1',
            'BUN_RUNTIME_TRANSPILER_CACHE_PATH=0',
            '/usr/local/bin/bun', '/mnt/benchmark/pi-workload.mjs',
            ...(mode === 'copilot' ? ['--copilot'] : [])], { env, stdout: 'pipe', stderr: 'pipe' });
        const timer = setTimeout(() => child.kill('SIGKILL'), 240000);
        const [stdout, stderr, exit] = await Promise.all([
            new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited]);
        clearTimeout(timer);
        // Do not relay arbitrary provider errors or responses to persistent logs.
        if (exit !== 0) {
            if (mode === 'offline') console.error(stderr);
            throw Error(`pi benchmark run ${run} failed (exit ${exit}, ${stderr.length} stderr bytes)`);
        }
        const result = JSON.parse(stdout.trim());
        console.log(JSON.stringify({ run, hostElapsedMs: performance.now() - start, ...result }));
    }
} finally { rmSync(stage, { recursive: true, force: true }); }

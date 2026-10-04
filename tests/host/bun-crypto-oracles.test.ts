import { expect, test } from 'bun:test';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const ish = process.env.ISH_BIN, root = process.env.ROOTFS;
for (const [name, marker, count] of [
    ['bun-crypto-records', 'CRYPTO_RECORD_OK', 24],
    ['bun-key-exchange', 'KEY_EXCHANGE_OK', 6],
    ['bun-mlkem', 'MLKEM_KEYGEN_OK', 6],
] as const) test.skipIf(!ish || !root)(`${name} agrees with independent native Node crypto`, () => {
    const stage = mkdtempSync(join(tmpdir(), 'linuxkit-crypto-oracle-'));
    try {
        const script = resolve(import.meta.dir, '../arm64/benchmarks', name + '.mjs');
        const oracle = Bun.spawnSync([process.env.NODE_BIN || 'node', script, '--oracle'], { timeout: 30000 });
        expect(oracle.exitCode).toBe(0);
        expect(oracle.stderr.toString()).toBe('');
        const fixture = join(stage, 'oracle.json');
        writeFileSync(fixture, oracle.stdout);
        const run = Bun.spawnSync([resolve(ish!), '-f', resolve(root!), '/usr/bin/env',
            'BUN_JSC_useJIT=0', 'BUN_JSC_numberOfGCMarkers=1', 'BUN_JSC_useConcurrentGC=0',
            '/usr/local/bin/bun', '/mnt/crypto-script', '/mnt/crypto-oracle'], {
            env: { ...process.env, ISH_JIT: '0',
                ISH_BIND_MOUNTS: `/mnt/crypto-script=${script}:ro,/mnt/crypto-oracle=${fixture}:ro` },
            timeout: 120000,
        });
        expect(run.exitCode).toBe(0);
        expect(run.signalCode).toBeFalsy();
        expect(run.stdout.toString().split('\n').filter(line => line.startsWith(marker))).toHaveLength(count);
    } finally { rmSync(stage, { recursive: true, force: true }); }
}, 180000);

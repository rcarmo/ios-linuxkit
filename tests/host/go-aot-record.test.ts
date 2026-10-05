import { expect, test } from 'bun:test';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const recorder = process.env.GO_AOT_RECORDER, root = process.env.ROOTFS;
test.skipIf(!recorder || !root)('exact Go recording excludes gofmt despite its path prefix', () => {
    const stage = mkdtempSync(join(tmpdir(), 'go-aot-exact-'));
    try {
        const record = join(stage, 'go.jsonl');
        const run = Bun.spawnSync([resolve(recorder!), '-f', resolve(root!), '/bin/sh', '-ec',
            '/usr/lib/go/bin/go version; printf "package probe\\n" | /usr/lib/go/bin/gofmt'], {
            env: { ...process.env, ISH_JIT: '1', ISH_JIT_PIC: '1',
                ISH_JIT_RECORD: record, ISH_JIT_RECORD_MOD: '/usr/lib/go/bin/go',
                ISH_JIT_RECORD_MOD_EXACT: '1', ISH_AOT_FAMILY: '0' },
            timeout: 300000,
        });
        expect(run.exitCode).toBe(0);
        expect(run.signalCode).toBeFalsy();
        expect(run.stdout.toString()).toContain('package probe');
        const rows = readFileSync(record, 'utf8').trim().split('\n').map(line => JSON.parse(line));
        expect(rows.shift()?.header).toBeTruthy();
        expect(rows.length).toBeGreaterThan(0);
        expect([...new Set(rows.map(row => row.mod))]).toEqual(['/usr/lib/go/bin/go']);
    } finally {
        rmSync(stage, { recursive: true, force: true });
    }
}, 360000);

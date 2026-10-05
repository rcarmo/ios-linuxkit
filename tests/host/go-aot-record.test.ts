import { expect, test } from 'bun:test';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const recorder = process.env.GO_AOT_RECORDER, root = process.env.ROOTFS;
test.skipIf(!recorder || !root).each([false, true])('exact Go recording excludes gofmt (target-only=%s)', targetOnly => {
    const stage = mkdtempSync(join(tmpdir(), 'go-aot-exact-'));
    try {
        const record = join(stage, 'go.jsonl');
        const run = Bun.spawnSync([resolve(recorder!), '-f', resolve(root!), '/bin/sh', '-ec',
            '/usr/lib/go/bin/go version; printf "package probe\\n" | /usr/lib/go/bin/gofmt'], {
            env: { ...process.env, ISH_JIT: '1', ISH_JIT_PIC: '1',
                ISH_JIT_RECORD: record, ISH_JIT_RECORD_MOD: '/usr/lib/go/bin/go',
                ISH_JIT_RECORD_MOD_EXACT: '1', ISH_AOT_FAMILY: '0',
                ISH_JIT_RECORD_TARGET_ONLY: targetOnly ? '1' : '0',
                ISH_JIT_MAP: join(stage, 'map.txt') },
            timeout: 300000,
        });
        expect(run.exitCode).toBe(0);
        expect(run.signalCode).toBeFalsy();
        expect(run.stdout.toString()).toContain('package probe');
        const rows = readFileSync(record, 'utf8').trim().split('\n').map(line => JSON.parse(line));
        expect(rows.shift()?.header?.region_exhausted).toBe(false);
        expect(rows.length).toBeGreaterThan(0);
        expect([...new Set(rows.map(row => row.mod))]).toEqual(['/usr/lib/go/bin/go']);
        const modules = readFileSync(join(stage, 'map.txt'), 'utf8').split('\n')
            .filter(line => line.startsWith('M ')).map(line => line.split(' '));
        const go = modules.find(row => row[6] === '/usr/lib/go/bin/go');
        const gofmt = modules.find(row => row[6] === '/usr/lib/go/bin/gofmt');
        expect(Number(go?.[4])).toBeGreaterThan(0);
        expect(Number(gofmt?.[2])).toBeGreaterThan(0);
        if (targetOnly) {
            expect(modules.filter(row => row[6] !== '/usr/lib/go/bin/go')
                .every(row => Number(row[4]) === 0 && Number(row[5]) === 0)).toBe(true);
        } else {
            expect(Number(gofmt?.[4])).toBeGreaterThan(0);
        }
    } finally {
        rmSync(stage, { recursive: true, force: true });
    }
}, 360000);

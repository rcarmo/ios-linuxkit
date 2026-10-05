import { expect, test } from 'bun:test';
import { resolve } from 'node:path';

const ish = process.env.ISH_BIN, root = process.env.ROOTFS;
test.skipIf(!ish || !root)('bundled Go formats, compiles, assembles, links and vets without downloads', () => {
    const fixture = resolve(import.meta.dir, '../arm64/benchmarks/go-aot');
    const run = Bun.spawnSync([resolve(ish!), '-f', resolve(root!), '/bin/sh', '/mnt/go-aot/build.sh'], {
        env: { ...process.env, ISH_JIT: process.env.ISH_JIT || '0', ISH_JIT_STATS: '1',
            ISH_BIND_MOUNTS: `/mnt/go-aot=${fixture}:ro` },
        timeout: 3600000,
    });
    const output = run.stdout.toString() + run.stderr.toString();
    expect(run.exitCode).toBe(0);
    expect(run.signalCode).toBeFalsy();
    expect(output).toContain('GO_AOT_BUILD_OK');
    if (process.env.GO_AOT_NO_EMIT === '1') {
        expect(output).toContain('JIT(AOT-only, no runtime emission)');
        expect(output).toContain('segments 0, units 0, code 0 KB');
        expect(output).toMatch(/AOT [1-9][0-9]* \(9 images\)/);
    }
}, 3660000);

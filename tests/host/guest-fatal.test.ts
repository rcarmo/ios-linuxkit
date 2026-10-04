import { expect, test } from 'bun:test';
import { resolve } from 'node:path';

const ish = process.env.ISH_BIN, root = process.env.ROOTFS;
test.skipIf(!ish || !root)('fatal guest signals retain wait status and do not terminate the parent shell', () => {
    const command = `for sig in 4 5 6 7 11; do
        /bin/sh -c "kill -$sig \\$\\$"
        status=$?
        test "$status" -eq "$((128 + sig))" || exit 1
        echo signal-$sig-status-$status
    done
    echo GUEST_FATAL_STATUS_OK`;
    const result = Bun.spawnSync([resolve(ish!), '-f', resolve(root!), '/bin/sh', '-c', command], {
        env: { ...process.env, ISH_JIT: '0' }, timeout: 30000,
    });
    expect(result.exitCode).toBe(0);
    expect(result.signalCode).toBeFalsy();
    for (const sig of [4, 5, 6, 7, 11]) {
        expect(result.stdout.toString()).toContain(`signal-${sig}-status-${128 + sig}`);
        expect(result.stderr.toString()).toContain(`GUEST_FATAL: sig=${sig}`);
    }
    expect(result.stdout.toString()).toContain('GUEST_FATAL_STATUS_OK');
});

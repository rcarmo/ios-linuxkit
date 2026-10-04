#!/usr/bin/env bun
// Test real Linux emitter/handler at O0 and O2; no Apple platform shims.
import { resolve, join } from 'node:path';
import { mkdirSync, mkdtempSync } from 'node:fs';
const root = resolve(import.meta.dir, '../../..');
const build = resolve(process.env.BUILD_DIR || join(root, 'build-arm64-native-release'));
const out = process.env.EVIDENCE_DIR || mkdtempSync('/workspace/tmp/ish-native-aot-');
mkdirSync(out, { recursive: true });
function run(args: string[]) {
    const r = Bun.spawnSync(args, { cwd: root, stdout: 'inherit', stderr: 'inherit' });
    if (r.exitCode !== 0) throw new Error(`${args[0]} exited ${r.exitCode}`);
}
function slice(source: string, start: string, end: string) {
    const a = source.indexOf(start), b = source.indexOf(end, a);
    if (a < 0 || b < a) throw new Error(`source boundary: ${start}`);
    return source.slice(a, b);
}
const main = await Bun.file(join(root, 'main.c')).text();
await Bun.write(join(out, 'cli-recovery.c'), '#include "platform/native_fault.h"\n#include "platform/native_fault.c"\n#include "platform/native_fault_app.c"\n' + slice(main, 'static void crash_handler(', 'static struct termios saved_termios'));
const dispatch = await Bun.file(join(root, 'asbestos/asbestos.c')).text();
const tlb = await Bun.file(join(root, 'emu/tlb.c')).text();
await Bun.write(join(out, 'dispatch-recovery.c'), `
${slice(tlb, 'void tlb_flush(', '\nvoid tlb_free(')}
${slice(dispatch, 'static inline unsigned asbestos_invalidate_gen_load(', '\nstatic int cpu_step_to_interrupt(')}
static int dispatch_after_fiber(int interrupt, struct fiber_frame *frame,
        struct tlb *tlb, struct asbestos *asbestos, unsigned *retries) {
    struct fiber_block **cache=tlb->block_cache;
    unsigned crash_retry_count=*retries;
${slice(dispatch, '        if (interrupt == INT_JIT_CRASH)', '        // Guest writes may modify code')}
    *retries=crash_retry_count;
    return interrupt;
}
`);
for (const opt of ['-O0', '-O2']) {
    const flags = ['-std=gnu11', opt, '-g', '-DGUEST_ARM64=1', '-DISH_JIT=1',
        '-DISH_JIT_NO_EMIT=1', '-I'+root, '-I'+build, '-I'+out,
        '-ffunction-sections', '-fdata-sections', '-Wno-unused-function', '-Wl,--gc-sections', '-pthread'];
    const layout = join(out, 'layout' + opt);
    run([process.env.CC || 'clang', ...flags, join(import.meta.dir, 'layout.c'),
        '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=mmap', '-o', layout]);
    for (const mode of ['empty', 'off', 'init', 'prepare']) run(['timeout', '-k', '2', '30', layout, mode]);
    for (const [name, sources] of [
        ['restart', ['restart.c', 'restart-call.S']],
        ['preservation', ['preservation.c', 'call.S', 'diff-call.S']],
    ] as const) {
        const exe = join(out, name + opt);
        run([process.env.CC || 'clang', ...flags, ...sources.map(s => join(import.meta.dir, s)), '-o', exe]);
        run(['timeout', '-k', '2', '120', exe]);
    }
}
console.log(`native-aot-emitter-gate-ok: ${out}`);

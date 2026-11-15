import { test, expect } from 'bun:test';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { tmpdir, arch } from 'node:os';

const root = resolve(import.meta.dir, '../../..');
const ordinary = process.env.PROFILE_GADGET_BIN || join(root, 'build-arm64-linux/ish');
const exact = process.env.PROFILE_EXACT_BIN;
for (const [mode, bin] of [['ordinary', ordinary], ['exact', exact]] as const) {
 const available = arch() === 'arm64' && !!bin && existsSync(bin);
 (available ? test : test.skip)(`real gadget ${mode} build writes module offsets on guest exit`, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'ish-prof-gadget-'));
  try {
   const commands = JSON.parse(readFileSync(join(dirname(bin!), 'compile_commands.json'), 'utf8'));
   expect(commands.some((r: any) => r.file.endsWith('/jit.c'))).toBe(false);
   for (const suffix of ['asbestos.c', 'control.S', 'entry.S']) {
    const command = commands.find((r: any) => r.file.endsWith(suffix)).command;
    if (mode === 'exact') {
     expect(command).toContain('-DDISABLE_BLOCK_CHAINING=1');
     expect(command).toContain('-DDISABLE_RET_CACHE=1');
    } else {
     expect(command).not.toContain('-DDISABLE_BLOCK_CHAINING');
     expect(command).not.toContain('-DDISABLE_RET_CACHE');
    }
   }
   const asm = join(dir, 'guest.S'), guest = join(dir, 'guest');
   await Bun.write(asm, `.text
.global _start,loop,return_site,done,helper
_start:
 mov x19, #5
 b loop
loop:
 bl helper
return_site:
 subs x19, x19, #1
 b.ne loop
done:
 mov x0, #0
 mov x8, #93
 svc #0
helper:
 nop
 ret
`);
   const cc = Bun.spawnSync(['aarch64-linux-gnu-gcc', '-nostdlib', '-static', '-Wl,-Ttext=0x401000,-e,_start,-z,max-page-size=4096', asm, '-o', guest], { stdout: 'pipe', stderr: 'pipe' });
   expect(cc.stderr.toString()).toBe('');
   expect(cc.exitCode).toBe(0);
   const profile = join(dir, 'profile'), map = join(dir, 'jit-map');
   // Exercise real defaults, independent of inherited diagnostic overrides.
   const env = { ...process.env };
   delete env.ISH_ARM64_EAGER_PRECHAIN;
   delete env.ISH_ARM64_EAGER_PRECHAIN_INCOMING;
   const run = Bun.spawnSync(['timeout', '-k', '1', '10', bin!, '-r', dir, '/guest'], {
    env: { ...env, ISH_BLOCK_PROF_FILE: profile, ISH_JIT: '0', ISH_JIT_MAP: map, ISH_ARM64_BLOCK_STATS: '1' },
    stdout: 'pipe', stderr: 'pipe',
   });
   if (run.exitCode !== 0) throw new Error(`${mode}: exit ${run.exitCode}; ${run.stderr.toString()}`);
   expect(existsSync(map)).toBe(false);
   const statsLine = run.stderr.toString().split('\n').find(l => l.startsWith('ARM64_BLOCK_STATS '))!;
   expect(statsLine).toBeDefined();
   const stats = Object.fromEntries([...statsLine.matchAll(/(\w+)=(\d+)/g)].map(m => [m[1], Number(m[2])]));
   for (const key of ['prechain_attempts', 'prechain_patches', 'prechain_outgoing_attempts', 'prechain_outgoing_patches', 'prechain_incoming_attempts', 'prechain_incoming_patches']) {
    if (mode === 'exact') expect(stats[key]).toBe(0);
    else expect(stats[key]).toBeGreaterThan(0);
   }
   if (mode === 'exact') expect(stats.chain_patches).toBe(0);
   const text = readFileSync(profile, 'utf8');
   expect(text).toContain('# dispatch profile');
   expect(text).toContain('/guest ');
   // Derive file offsets from ELF program headers, not virtual-address guesses.
   const elf = readFileSync(guest), phoff = Number(elf.readBigUInt64LE(32));
   const segments: { off: number; va: number; len: number }[] = [];
   for (let i = 0; i < elf.readUInt16LE(56); i++) {
    const p = phoff + i * elf.readUInt16LE(54);
    if (elf.readUInt32LE(p) === 1) segments.push({ off: Number(elf.readBigUInt64LE(p + 8)), va: Number(elf.readBigUInt64LE(p + 16)), len: Number(elf.readBigUInt64LE(p + 32)) });
   }
   const nm = Bun.spawnSync(['aarch64-linux-gnu-nm', '-n', guest], { stdout: 'pipe', stderr: 'pipe' });
   expect(nm.exitCode).toBe(0);
   const rows = text.split('\n').filter(l => l.startsWith('/guest ')).map(l => l.split(/\s+/));
   // gen.c folds the initial direct B into the first block, so that block
   // includes the first BL; only the subsequent four enter at loop.
   const counts: Record<string, number> = { _start: 1, loop: 4, return_site: 5, helper: 5, done: 1 };
   for (const [symbol, count] of Object.entries(counts)) {
    const line = nm.stdout.toString().split('\n').find(l => l.endsWith(` ${symbol}`))!;
    const va = parseInt(line, 16), seg = segments.find(s => va >= s.va && va < s.va + s.len)!;
    const off = va - seg.va + seg.off;
    const row = rows.find(r => parseInt(r[1], 16) === off);
    if (mode === 'exact') {
     expect(row).toBeDefined();
     if (Number(row![3]) !== count) throw new Error(`${symbol}: expected ${count}; profile:\n${text}`);
    }
   }
   const weighted = rows.reduce((sum, row) => sum + Number(row[2]) * Number(row[3]), 0);
   expect(weighted).toBeGreaterThan(0);
   if (mode === 'exact') {
    expect(weighted).toBe(30);
    // The compile-time prohibition must also defeat explicit runtime opt-ins.
    const forced = Bun.spawnSync(['timeout', '-k', '1', '10', bin!, '-r', dir, '/guest'], {
     env: { ...env, ISH_BLOCK_PROF_FILE: profile, ISH_JIT: '0', ISH_ARM64_EAGER_PRECHAIN: '1', ISH_ARM64_EAGER_PRECHAIN_INCOMING: '1', ISH_ARM64_BLOCK_STATS: '1' },
     stdout: 'pipe', stderr: 'pipe',
    });
    expect(forced.exitCode).toBe(0);
    expect(readFileSync(profile, 'utf8')).toBe(text);
    expect(forced.stderr.toString()).toContain('prechain_attempts=0 prechain_patches=0 prechain_outgoing_attempts=0 prechain_outgoing_patches=0 prechain_incoming_attempts=0 prechain_incoming_patches=0');
   } else {
    // Ordinary builds are dispatch samples, not exact executed counts.
    expect(weighted).toBeLessThan(30);
   }
   console.log(`${mode}: weighted=${weighted}, outgoing=${stats.prechain_outgoing_patches}, incoming=${stats.prechain_incoming_patches}`);
   // Profiling disabled produces neither diagnostics nor an output file.
   rmSync(profile);
   const disabled = Bun.spawnSync(['timeout', '-k', '1', '10', bin!, '-r', dir, '/guest'], {
    env: { ...env, ISH_BLOCK_PROF_FILE: '', ISH_JIT: '0' }, stdout: 'pipe', stderr: 'pipe',
   });
   expect(disabled.exitCode).toBe(0);
   expect(existsSync(profile)).toBe(false);
   expect(disabled.stderr.toString()).not.toContain('[block_prof]');
  } finally { rmSync(dir, { recursive: true, force: true }); }
 }, 30000);
}

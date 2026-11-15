import { test, expect } from 'bun:test';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';

const root = resolve(import.meta.dir, '../../..');
for (const jit of [false, true]) {
 test(`producer uses actual mapping offsets without JIT helpers (ISH_JIT=${jit})`, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'ish-profile-'));
  try {
   const source = readFileSync(join(root, 'asbestos/asbestos.c'), 'utf8');
   const begin = source.indexOf('#ifdef GUEST_ARM64\n// Optional dispatch profile.');
   const end = source.indexOf('// Stubs / debug hooks', begin);
   expect(begin).toBeGreaterThan(0);
   const output = join(dir, 'profile.txt');
   await Bun.write(join(dir, 'test.c'), `
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define GUEST_ARM64 1
${jit ? '#define ISH_JIT 1' : ''}
#define MAX_PATH 4096
#define PAGE(pc) ((pc) >> 12)
#define PGOFFSET(pc) ((pc) & 4095)
#define container_of(p,t,m) ((t *) ((char *) (p) - offsetof(t,m)))
typedef uint64_t addr_t;
struct mmu { uint64_t changes; };
struct mem { struct mmu mmu; };
struct tlb { struct mmu *mmu; };
struct fiber_block { uint64_t addr, end_addr; };
struct fd { int unused; };
struct data { struct fd *fd; size_t file_offset; const char *name; int jit_mod; };
struct pt_entry { struct data *data; size_t offset; };
static struct fd fd;
static struct data data = { &fd, 0x6000, NULL, 71 };
static struct pt_entry pt = { &data, 0x2000 };
static size_t real_page_size = 16384;
static int paths;
static struct pt_entry *mem_pt(struct mem *m, uint64_t page) {
 (void)m; return page == 9 ? NULL : &pt;
}
static int generic_getpath(struct fd *f, char *buf) {
 (void)f; paths++; strcpy(buf, "/lib/test module.so"); return 0;
}
${source.slice(begin, end)}
static struct mem mem;
static struct tlb tlb = { &mem.mmu };
static void *worker(void *unused) {
 (void)unused;
 struct fiber_block b = {0x5010, 0x5010 + 1199};
 for (int i=0; i<1000; i++) blk_prof_record(&tlb, &b);
 return NULL;
}
int main(int argc, char **argv) {
 if (argc > 1) {
  unsetenv("ISH_BLOCK_PROF_FILE");
  pthread_once(&blk_prof_once, blk_prof_init);
  struct fiber_block b = {0x1010,0x1013};
  blk_prof_record(&tlb,&b); dump_block_prof();
  return blk_prof || paths;
 }
 (void)argv;
 setenv("ISH_BLOCK_PROF_FILE", ${JSON.stringify(output)}, 1);
 pthread_once(&blk_prof_once, blk_prof_init);
 struct fiber_block a = {0x1010,0x1010+1199}, b = {0x5010,0x5010+1199};
 struct fiber_block single = {0x5020,0x5023}, variant = {0x5010,0x5013};
 struct fiber_block raw = {0x9010,0x9013};
 blk_prof_record(&tlb,&a); blk_prof_record(&tlb,&b);
 blk_prof_record(&tlb,&single); blk_prof_record(&tlb,&variant);
 if (paths != 2 || data.jit_mod != 71) return 2;
 blk_prof_record(&tlb,&raw);
 // Invalidate the one-page cache on a remap, even at the same guest PC.
 mem.mmu.changes++; pt.offset = 0x3000;
 blk_prof_record(&tlb,&single);
 mem.mmu.changes++; pt.offset = 0x2000;
 pthread_t threads[4];
 for(int i=0;i<4;i++) pthread_create(&threads[i],NULL,worker,NULL);
 for(int i=0;i<4;i++) pthread_join(threads[i],NULL);
 // Dump must not dereference freed mapping metadata.
 data.fd = NULL; data.name = NULL;
 dump_block_prof(); _Exit(0);
}
`);
   const cc = Bun.spawnSync(['clang', '-pthread', join(dir, 'test.c'), '-o', join(dir, 'test')], { stdout: 'pipe', stderr: 'pipe' });
   expect(cc.stderr.toString()).toBe('');
   expect(cc.exitCode).toBe(0);
   const run = Bun.spawnSync([join(dir, 'test')], { stdout: 'pipe', stderr: 'pipe' });
   expect(run.exitCode).toBe(0);
   const profile = readFileSync(output, 'utf8');
   expect(profile).toContain('/lib/test module.so 6010 300 4002');
   expect(profile).toContain('/lib/test module.so 6010 1 1');
   expect(profile).toContain('/lib/test module.so 6020 1 1');
   expect(profile).toContain('/lib/test module.so 7020 1 1');
   expect(profile).toContain('9010 1 1');
   expect(profile).toContain('dispatch profile');
   expect(run.stderr.toString()).toContain('DISABLE_BLOCK_CHAINING and DISABLE_RET_CACHE');
   expect(run.stderr.toString()).not.toContain('ISH_ARM64_EAGER_PRECHAIN');
   expect(run.stderr.toString()).toContain('ISH_NO_CHAIN/ISH_NO_RETCACHE are unsupported');
   const disabled = Bun.spawnSync([join(dir, 'test'), 'disabled'], { stdout: 'pipe', stderr: 'pipe' });
   expect(disabled.exitCode).toBe(0);
   expect(disabled.stderr.toString()).toBe('');
   const recording = join(dir, 'recording.jsonl'), keep = join(dir, 'keep.txt');
   await Bun.write(recording, [{ header: {} },
    { mod: '/lib/test module.so', off: 0x6010, segs: [{ words: [0, 0] }] },
    { mod: '/lib/test module.so', off: 0x6020, segs: [{ words: [0] }] },
   ].map(x => JSON.stringify(x)).join('\n') + '\n');
   const select = Bun.spawnSync(['python3', join(root, 'tools/jit_aot/hotblocks.py'), recording, output, '--keep', keep, '--coverage', '1'], { stdout: 'pipe', stderr: 'pipe' });
   expect(select.exitCode).toBe(0);
   expect(readFileSync(keep, 'utf8')).toBe('0x6010\n0x6020\n');
  } finally { rmSync(dir, { recursive: true, force: true }); }
 });
}

test('disabled dispatch does not initialize the profiler or resolve modules', () => {
 const source = readFileSync(join(root, 'asbestos/asbestos.c'), 'utf8');
 const record = source.slice(source.indexOf('static void blk_prof_record('), source.indexOf('// Stubs / debug hooks'));
 expect(record).not.toMatch(/pthread_once\s*\(/);
 expect(record).not.toContain('jit_prof_locate');
 expect(record).toContain('if (!blk_prof) return;');
});

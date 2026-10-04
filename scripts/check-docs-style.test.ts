import {test,expect} from 'bun:test';
import {inspect} from './check-docs-style';
test('flags internal project jargon without rejecting technical instructions',()=>{
 expect(inspect('This tranche completes the source ledger and acceptance gates.').some(x=>x.rule==='internal jargon')).toBe(true);
 expect(inspect('Build the app, verify its signature and test typing on the iPhone.\nUse `jit_layout_read` to inspect the runtime configuration.')).toEqual([]);
});
test('flags filler, manufactured contrast and generic headings',()=>{const r=inspect('## Overview\nIt is worth noting that this is seamless.\nThis is not a cache; it is the database.\nWe do not claim a speedup.\nThe work remains pending.');expect(r.some(x=>x.rule==='generic heading')).toBe(true);expect(r.some(x=>x.rule==='filler')).toBe(true);expect(r.some(x=>x.rule==='manufactured contrast')).toBe(true);expect(r.some(x=>x.rule==='editorial claim')).toBe(true);expect(r.some(x=>x.rule==='passive status')).toBe(true);});
test('preserves code, identifiers and direct technical limits',()=>{expect(inspect('The iOS app uses gadgets. Apple AOT device tests have not run.\nUse `--summary` for output.\n```sh\necho "honestly seamless"\n```\n> It is worth noting a quoted source.\n| Historical | seamless |')).toEqual([]);});

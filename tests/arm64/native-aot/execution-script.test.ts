import {test,expect} from 'bun:test';
import {executionScript} from './execution';
test('hardware-PC proof names images independently of order and decodes relative segment fields',()=>{
 const s=executionScript('bun');
 expect(s).toContain('&ish_aot_module_bun');
 expect(s).toContain('(char *)&t->seg + t->seg');
 expect(s).toContain('(char *)&$s->code + $s->code');
 expect(s).toContain('set $lo = $m->text_start');
 expect(s).toContain('set $hi = $m->text_end');
 expect(s).not.toContain('aot_images[');
 expect(s).toContain('ACTUAL_AOT_PC bun');
 expect(()=>executionScript('bad;quit')).toThrow();
});

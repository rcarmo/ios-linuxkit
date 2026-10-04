import fs from 'node:fs';
import promises from 'node:fs/promises';
import { join } from 'node:path';
const root = fs.mkdtempSync('/tmp/bun-remove-');
const outside = join(root, 'outside');
fs.mkdirSync(outside);
fs.writeFileSync(join(outside, 'sentinel'), 'keep');
try {
    for (const mode of ['sync', 'promise', 'callback']) {
        const tree = join(root, mode);
        fs.mkdirSync(join(tree, 'nested', 'deep'), { recursive: true });
        fs.writeFileSync(join(tree, 'nested', 'deep', 'file'), 'test');
        fs.symlinkSync(outside, join(tree, 'link'));
        fs.symlinkSync('/nonexistent-bun-remove-target', join(tree, 'dangling'));
        try { fs.unlinkSync(tree); throw Error('directory unlink unexpectedly succeeded'); }
        catch (error) { if (error.code !== 'EISDIR') throw error; }
        if (mode === 'sync') fs.rmSync(tree, { recursive: true, force: true });
        else if (mode === 'promise') await promises.rm(tree, { recursive: true, force: true });
        else await new Promise((resolve, reject) => fs.rm(tree, { recursive: true, force: true },
            error => error ? reject(error) : resolve()));
        if (fs.existsSync(tree) || fs.readFileSync(join(outside, 'sentinel'), 'utf8') !== 'keep')
            throw Error('recursive removal changed the symlink target or retained the tree');
        console.log(`BUN_REMOVE_OK ${mode}`);
    }
    const empty = join(root, 'empty');
    fs.mkdirSync(empty);
    fs.rmSync(empty, { recursive: true });
    fs.rmSync(empty, { recursive: true, force: true });
    const file = join(root, 'file');
    fs.writeFileSync(file, 'test');
    fs.rmSync(file);
    console.log('BUN_REMOVE_OK empty-missing-file');
} finally {
    fs.rmSync(root, { recursive: true, force: true });
}

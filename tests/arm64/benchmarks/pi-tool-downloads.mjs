import { mkdtempSync, readdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const agent = mkdtempSync('/tmp/pi-tool-downloads-');
process.env.PI_CODING_AGENT_DIR = agent;
process.env.PI_OFFLINE = '0';
try {
    const { ensureTool, getToolPath } = await import(
        '/opt/pi/node_modules/@earendil-works/pi-coding-agent/dist/utils/tools-manager.js');
    const tools = ['fd', 'rg'];
    for (const tool of tools) {
        if (getToolPath(tool)) throw Error(`${tool} already exists; use a fixture without system fd/ripgrep`);
    }
    const warnings = [];
    const paths = await Promise.all(tools.map(tool => ensureTool(tool, status => {
        console.log(`${tool}: ${status.message}`);
        if (status.type === 'warning') warnings.push(status.message);
    })));
    for (let i = 0; i < tools.length; i++) {
        if (paths[i] !== join(agent, 'bin', tools[i])) throw Error(`${tools[i]} download failed`);
        const run = Bun.spawnSync([paths[i], '--version'], { timeout: 30000 });
        if (run.exitCode !== 0) throw Error(`${tools[i]} cannot run: ${run.stderr.toString()}`);
        console.log(`PI_TOOL_DOWNLOAD_OK ${run.stdout.toString().trim()}`);
    }
    if (warnings.length) throw Error(warnings.join('\n'));
    const remaining = readdirSync(join(agent, 'bin')).sort();
    if (JSON.stringify(remaining) !== JSON.stringify(tools))
        throw Error(`download leftovers: ${remaining.join(', ')}`);
    console.log('PI_TOOL_CLEANUP_OK');
} finally {
    rmSync(agent, { recursive: true, force: true });
}

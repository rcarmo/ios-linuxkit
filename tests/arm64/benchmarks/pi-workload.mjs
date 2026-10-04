// Run with guest Bun after installing pi in /opt/pi. No real project files.
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const started = performance.now();
const modules = '/opt/pi/node_modules/@earendil-works';
const { Agent } = await import(`${modules}/pi-agent-core/dist/agent.js`);
const { createFauxCore, fauxAssistantMessage, fauxToolCall } =
    await import(`${modules}/pi-ai/dist/providers/faux.js`);
const { createReadTool } = await import(`${modules}/pi-coding-agent/dist/core/tools/read.js`);
const { createEditTool } = await import(`${modules}/pi-coding-agent/dist/core/tools/edit.js`);
const { createWriteTool } = await import(`${modules}/pi-coding-agent/dist/core/tools/write.js`);
const { createBashTool } = await import(`${modules}/pi-coding-agent/dist/core/tools/bash.js`);
const importMs = performance.now() - started;
const live = process.argv.includes('--copilot');
const cwd = await mkdtemp('/tmp/pi-benchmark-');
const source = 'export function sum(values) { return values.reduce((total, value) => total - value, 0); }\n';
const test = `import { sum } from './sum.js';
const values = JSON.parse(await Bun.file('./values.json').text());
if (sum(values) !== 32640 || sum([]) !== 0 || sum([-2, 3]) !== 1) throw Error('sum regression');
console.log('PI_WORKLOAD_OK');\n`;
const command = '/usr/local/bin/bun check.js';
const tools = [createReadTool(cwd), createEditTool(cwd), createWriteTool(cwd),
    createBashTool(cwd, { shellPath: '/bin/sh' })];
// Keep a live benchmark confined to synthetic files and one known test command.
for (const tool of tools) {
    const execute = tool.execute.bind(tool);
    tool.execute = async (id, args, ...rest) => {
        if (tool.name === 'bash') {
            if (args.command !== command) throw Error('benchmark command rejected');
        } else if (!['sum.js', 'values.json', 'check.js'].some(name =>
            resolve(cwd, args.path) === join(cwd, name))) {
            throw Error('benchmark path rejected');
        }
        return execute(id, args, ...rest);
    };
}
let requests = 0;
let model, stream;
if (live) {
    const { githubCopilotProvider } = await import(`${modules}/pi-ai/dist/providers/github-copilot.js`);
    const provider = githubCopilotProvider();
    model = provider.getModels().find(model => model.id === 'gpt-4.1');
    if (!model) throw Error('Copilot gpt-4.1 not in pi catalog');
    const auth = JSON.parse(await readFile('/mnt/pi-auth/request.json', 'utf8'));
    model = { ...model, baseUrl: auth.baseUrl };
    stream = (model, context, options) => {
        if (++requests > 12) throw Error('benchmark request limit');
        return provider.streamSimple(model, context, { ...options, apiKey: auth.apiKey, maxTokens: 2048 });
    };
} else {
    const faux = createFauxCore({ models: [{ id: 'offline-benchmark' }] });
    const calls = [
        ['read', { path: 'sum.js' }],
        ['read', { path: 'values.json' }],
        ['edit', { path: 'sum.js', edits: [{ oldText: 'total - value', newText: 'total + value' }] }],
        ['write', { path: 'check.js', content: test }],
        ['bash', { command }],
    ];
    faux.setResponses([...calls.map(([name, args], index) => fauxAssistantMessage(
        fauxToolCall(name, args, { id: `call-${index}` }), { stopReason: 'toolUse' })),
        fauxAssistantMessage('PI_WORKLOAD_OK')]);
    model = faux.getModel();
    stream = (...args) => { requests++; return faux.streamSimple(...args); };
}
try {
    await writeFile(join(cwd, 'sum.js'), source);
    await writeFile(join(cwd, 'values.json'), JSON.stringify(Array.from({ length: 256 }, (_, i) => i)));
    const agent = new Agent({ streamFn: stream, initialState: {
        model, tools, systemPrompt: `Work only in ${cwd}. Use read, edit, write and bash tools.`,
    } });
    const toolStarts = new Map();
    const timings = [];
    let toolErrors = 0, testPassed = false;
    agent.subscribe(event => {
        if (event.type === 'tool_execution_start') toolStarts.set(event.toolCallId, performance.now());
        if (event.type === 'tool_execution_end') {
            timings.push({ tool: event.toolName, ms: performance.now() - toolStarts.get(event.toolCallId) });
            if (event.isError) toolErrors++;
            if (event.toolName === 'bash' && !event.isError &&
                event.result.content.some(c => c.type === 'text' && c.text.includes('PI_WORKLOAD_OK'))) testPassed = true;
        }
    });
    const workloadStarted = performance.now();
    const watchdog = setTimeout(() => agent.abort(), 180000);
    try {
        await agent.prompt(`Read sum.js and values.json. Fix the sum bug using edit. Write check.js to test
the sum of values.json equals 32640, sum([]) equals 0, and sum([-2,3]) equals 1.
Print PI_WORKLOAD_OK on success. Run exactly this bash command: ${command}.
Do not access any other files. Stop after the test passes.`);
    } finally { clearTimeout(watchdog); }
    if (!testPassed || toolErrors || agent.state.errorMessage) throw Error('pi workload failed');
    console.log(JSON.stringify({ benchmark: 'pi-synthetic-tools', mode: live ? 'copilot' : 'offline-faux',
        bun: Bun.version, pi: '1.0.2', javascriptJitDisabled: process.env.BUN_JSC_useJIT === '0',
        importMs, workloadMs: performance.now() - workloadStarted,
        totalMs: performance.now() - started, requests, timings, toolErrors, testPassed }));
} finally {
    await rm(cwd, { recursive: true, force: true });
}

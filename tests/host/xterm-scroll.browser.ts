// PLAYWRIGHT_MODULE=/absolute/path/to/playwright/index.mjs bun tests/host/xterm-scroll.browser.ts
import { strict as assert } from 'node:assert';
import { resolve } from 'node:path';
import { mkdirSync } from 'node:fs';

const module = process.env.PLAYWRIGHT_MODULE;
if (!module) throw Error('Set PLAYWRIGHT_MODULE to an installed Playwright index.mjs');
const { webkit } = await import(module);
const root = resolve(import.meta.dir, '../../app/terminal');
const evidence = resolve(process.env.TERMINAL_TEST_EVIDENCE || '/tmp/ios-linuxkit-terminal-scroll');
mkdirSync(evidence, { recursive: true });
const server = Bun.serve({ hostname: '127.0.0.1', port: 0, async fetch(request) {
    const path = resolve(root, '.' + new URL(request.url).pathname);
    if (!path.startsWith(root + '/')) return new Response('Not found', { status: 404 });
    const file = Bun.file(path);
    if (!await file.exists()) return new Response('Not found', { status: 404 });
    if (path.endsWith('/xterm-term-bridge.js')) {
        const probe = `const OriginalTerminal = window.xtermModules.Terminal;
window.xtermModules.Terminal = class extends OriginalTerminal {
    constructor(options) { super(options); window.testTerminal = this; }
};\n`;
        return new Response(probe + await file.text(), { headers: { 'Content-Type': 'text/javascript' } });
    }
    return new Response(file);
} });
const browser = await webkit.launch();
try {
    for (const viewport of [{ width: 393, height: 769 }, { width: 1024, height: 997 }]) {
        const page = await browser.newPage({ viewport, deviceScaleFactor: 3, hasTouch: true });
        const errors: string[] = [];
        page.on('pageerror', (error: Error) => errors.push(error.message));
        await page.addInitScript(() => {
            (window as any).bridgeMessages = [];
            (window as any).webkit = { messageHandlers: new Proxy({}, { get: (_, name) => ({
                postMessage(value: any) { (window as any).bridgeMessages.push({ name, value }); },
            }) }) };
        });
        await page.goto(`http://127.0.0.1:${server.port}/xterm-term.html`);
        await page.waitForFunction(() => (window as any).bridgeMessages.some((m: any) => m.name === 'load'));
        const settle = () => page.evaluate(() => new Promise<void>(resolve =>
            requestAnimationFrame(() => requestAnimationFrame(() => requestAnimationFrame(() => resolve())))));
        const write = (text: string) => page.evaluate((text: string) => new Promise<void>(resolve => {
            const term = (window as any).testTerminal;
            const event = term.onWriteParsed(() => { event.dispose(); resolve(); });
            (window as any).exports.write(text);
        }), text);
        const state = () => page.evaluate(() => {
            const term = (window as any).testTerminal;
            const buffer = term.buffer.active;
            const height = term._core._renderService.dimensions.css.cell.height;
            return { rows: term.rows, baseY: buffer.baseY, viewportY: buffer.viewportY, cellHeight: height,
                firstLine: buffer.getLine(buffer.viewportY)?.translateToString(true),
                viewportHeight: document.getElementById('terminal')!.clientHeight,
                messages: (window as any).bridgeMessages };
        });
        await write(Array.from({ length: 10500 }, (_, i) => `line-${i}\r\n`).join(''));
        await settle();
        let before = await state();
        let scrollHeight = before.messages.filter((m: any) => m.name === 'newScrollHeight').at(-1).value;
        assert.equal(scrollHeight - before.viewportHeight, before.baseY * before.cellHeight);
        await page.evaluate((top: number) => {
            (window as any).bridgeMessages.length = 0;
            (window as any).exports.newScrollTop(top);
        }, (before.baseY - 300) * before.cellHeight + 0.3);
        await settle();
        before = await state();
        assert.equal(before.messages.filter((m: any) => m.name === 'newScrollTop').length, 0);
        await write(Array.from({ length: 100 }, (_, i) => `new-${i}\r\n`).join(''));
        await settle();
        const after = await state();
        assert.equal(after.firstLine, before.firstLine, 'trimmed output must preserve the line being read');
        assert.equal(after.viewportY, before.viewportY - 100);

        await page.evaluate(() => (window as any).exports.scrollToBottom());
        await settle();
        await page.setViewportSize({ width: viewport.width, height: 401 });
        await settle();
        const keyboard = await state();
        assert.ok(keyboard.rows * keyboard.cellHeight <= keyboard.viewportHeight, 'rows must fit above keyboard');
        assert.equal(keyboard.viewportY, keyboard.baseY, 'bottom must remain reachable after keyboard resize');
        scrollHeight = keyboard.messages.filter((m: any) => m.name === 'newScrollHeight').at(-1).value;
        assert.equal(scrollHeight - keyboard.viewportHeight, keyboard.baseY * keyboard.cellHeight);
        await page.screenshot({ path: resolve(evidence, `keyboard-${viewport.width}.png`) });

        await write('\x1b[?1049hfull-screen application');
        await settle();
        const alternate = await state();
        assert.equal(alternate.baseY, 0);
        assert.equal(alternate.messages.filter((m: any) => m.name === 'newScrollHeight').at(-1).value, alternate.viewportHeight);
        await write('\x1b[?1049l'); await settle();
        assert.equal(errors.length, 0, errors.join('\n'));
        console.log(JSON.stringify({ viewport, status: 'pass', rowsWithKeyboard: keyboard.rows,
            retainedHistoryLine: after.firstLine, evidence }));
        await page.close();
    }
} finally {
    await browser.close();
    server.stop(true);
}

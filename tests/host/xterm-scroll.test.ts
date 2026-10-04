import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { runInNewContext } from 'node:vm';

const source = readFileSync(resolve(import.meta.dir, '../../app/terminal/xterm-term-bridge.js'), 'utf8');

async function harness() {
    const frames: (() => void)[] = [];
    const messages: { name: string; value: any }[] = [];
    const element = { clientHeight: 307, classList: { toggle() {} }, addEventListener() {} };
    let terminal: any;
    let fits = 0;
    class Terminal {
        cols = 80;
        rows = 30;
        options: any;
        buffer = { active: { baseY: 100, viewportY: 100 } };
        _core = { _renderService: { dimensions: { css: { cell: { width: 6, height: 10 } } } } };
        onScrollCallback = () => {};
        constructor(options: any) { this.options = options; terminal = this; }
        loadAddon() {}
        onData() {}
        onResize() {}
        onCursorMove() {}
        onScroll(fn: () => void) { this.onScrollCallback = fn; }
        open() {}
        refresh() {}
        write(_data: Uint8Array, done: () => void) { done(); }
        scrollToLine(line: number) { this.buffer.active.viewportY = line; this.onScrollCallback(); }
        scrollToBottom() { this.scrollToLine(this.buffer.active.baseY); }
        clear() { this.buffer.active.baseY = this.buffer.active.viewportY = 0; }
    }
    class FitAddon { fit() { fits++; } }
    class Addon {}
    let resize: () => void = () => {};
    const window: any = {
        xtermModules: { Terminal, FitAddon, WebLinksAddon: Addon, LigaturesAddon: Addon },
        CanvasAddon: { CanvasAddon: Addon },
        webkit: { messageHandlers: new Proxy({}, { get: (_, name: string) => ({
            postMessage(value: any) { messages.push({ name, value }); },
        }) }) },
    };
    runInNewContext(source, {
        window, document: { getElementById: () => element, documentElement: { style: { setProperty() {} } } },
        Uint8Array, requestAnimationFrame: (fn: () => void) => { frames.push(fn); return frames.length; },
        ResizeObserver: class { constructor(fn: () => void) { resize = fn; } observe() {} },
    });
    await Promise.resolve();
    await Promise.resolve();
    function flush() {
        for (let rounds = 0; frames.length && rounds < 10; rounds++) {
            for (const fn of frames.splice(0)) fn();
        }
        expect(frames).toHaveLength(0);
    }
    flush();
    messages.length = 0;
    return { window, terminal, element, messages, flush, resize, fits: () => fits };
}

test('native range includes the partial row and reaches the exact buffer bottom', async () => {
    const h = await harness();
    h.terminal.buffer.active.baseY++;
    h.window.exports.write('output'); h.flush();
    const height = h.messages.find(m => m.name === 'newScrollHeight')?.value;
    expect(height - h.element.clientHeight).toBe(1010);
});

test('fractional UIKit scrolling is acknowledged without snapping its offset', async () => {
    const h = await harness();
    h.window.exports.newScrollTop(123.7); h.flush();
    expect(h.terminal.buffer.active.viewportY).toBe(12);
    expect(h.messages.filter(m => m.name === 'newScrollTop')).toHaveLength(0);
    h.window.exports.write('more output'); h.flush();
    expect(h.messages.filter(m => m.name === 'newScrollTop')).toHaveLength(0);
});

test('output grows scrollback without moving a reader away from history', async () => {
    const h = await harness();
    h.window.exports.newScrollTop(200); h.flush();
    h.terminal.buffer.active.baseY += 10;
    h.window.exports.write('output'); h.flush();
    expect(h.terminal.buffer.active.viewportY).toBe(20);
    expect(h.messages.filter(m => m.name === 'newScrollTop')).toHaveLength(0);
    expect(h.messages.find(m => m.name === 'newScrollHeight')?.value).toBe(1407);
});

test('trimmed history sends a corrected offset rather than acknowledging a stale request', async () => {
    const h = await harness();
    h.window.exports.newScrollTop(200);
    h.terminal.buffer.active.viewportY = 17;
    h.window.exports.write('trimmed output'); h.flush();
    expect(h.messages.find(m => m.name === 'newScrollTop')?.value).toBe(170);
});

test('scroll requests clamp to bounds and reject nonfinite offsets or absent metrics', async () => {
    const h = await harness();
    h.window.exports.newScrollTop(-500); h.flush();
    expect(h.terminal.buffer.active.viewportY).toBe(0);
    h.window.exports.newScrollTop(50000); h.flush();
    expect(h.terminal.buffer.active.viewportY).toBe(100);
    h.window.exports.newScrollTop(Infinity);
    h.window.exports.newScrollTop(NaN);
    h.terminal._core._renderService.dimensions.css.cell.height = 0;
    h.window.exports.newScrollTop(10); h.flush();
    expect(h.terminal.buffer.active.viewportY).toBe(100);
});

test('keyboard resizing coalesces into one fit on the next frame', async () => {
    const h = await harness();
    const before = h.fits();
    h.element.clientHeight = 203;
    h.resize(); h.resize(); h.resize(); h.flush();
    expect(h.fits() - before).toBe(1);
    expect(h.messages.find(m => m.name === 'newScrollHeight')?.value).toBe(1203);
});

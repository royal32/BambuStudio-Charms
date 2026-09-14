// Run: node --experimental-vm-modules --test tests/bambu_connect_bridge.test.cjs
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../resources/scripts/bambu_connect_bridge.js'), 'utf8');

async function fixture({ disabled = false, warning = false, cancelled = false, normalize = false, importFails = false, startupRedirect = false } = {}) {
    let sends = 0, imports = 0, navigations = 0;
    const device = { id: 'desk', amsFilament: { amsItems: [{ trays: [
        { type: 'ams', amsId: '0', slotId: '2', extruderId: 0 }
    ] }], externalTrays: [{ type: 'external', extruderId: 0 }] } };
    const state = { printOptions: {}, filamentTrayMappings: [], sending: false };
    const modal = { get visible() { return store.modalSource === 'print'; }, title: 'Send to print', okButtonProps: { disabled },
        onOk: async () => {
            sends++;
            if (!warning) store.modalSource = undefined;
            if (!warning && !cancelled) context.window.location.hash = '#/devices/desk';
        } };
    const root = { firstElementChild: { '__reactFiber$test': { memoizedProps: modal } } };
    let nativeFrames = 0, nativeCancellations = 0;
    const context = vm.createContext({ File, Uint8Array, setTimeout, clearTimeout, performance,
        window: { location: { hash: '#/print' },
            requestAnimationFrame: () => ++nativeFrames,
            cancelAnimationFrame: () => ++nativeCancellations },
        document: { visibilityState: 'hidden', getElementById: () => root } });
    const store = {
        state, modalSource: undefined,
        setModalSource(value) { this.modalSource = value; },
        closeFile() { state.gcode3MF = undefined; state.activePlate = undefined; },
        async importFile() {
            imports++;
            if (importFails) return { success: false };
            state.gcode3MF = { allGcodeData: [1, 2].map(index => ({ sliceInfoConfig: { index: String(index) } })) };
            state.activePlate = state.gcode3MF.allGcodeData[0];
            state.filamentTrayMappings = [{ sliceConfigFilament: { id: '1' } }];
            return { success: true };
        },
        setActivePlate(p) { state.activePlate = p; },
        openPrintModal(source, selected) {
            this.modalSource = source; state.deviceId = selected?.id ?? 'other';
            if (startupRedirect) Promise.resolve().then(() => { context.window.location.hash = '#/devices'; });
        },
        setPrintOptions(options) { Object.assign(state.printOptions, options); if (normalize) state.printOptions.timelapse = false; },
        setFilamentTrayMapping(id, tray) { state.filamentTrayMappings.find(m => m.sliceConfigFilament.id === id).apiTray = { matchedTray: tray }; }
    };
    const io = new vm.SyntheticModule(['a2', 'Y'], function () {
        this.setExport('a2', { invoke: async () => ({ file: { data: [1, 2, 3] } }) });
        this.setExport('Y', x => x);
    }, { context });
    const router = new vm.SyntheticModule(['router'], function () {
        this.setExport('router', { navigate: async () => {
            if (++navigations === 1) store.modalSource = undefined;
            context.window.location.hash = '#/print';
        } });
    }, { context });
    for (const module of [io, router]) { await module.link(() => {}); await module.evaluate(); }
    const module = new vm.SourceTextModule('export default ' + source, { context,
        importModuleDynamically: specifier => specifier.includes('router-') ? router : io });
    await module.link(() => {}); await module.evaluate();
    const adapter = module.namespace.default(() => store, () => ({ devices: [device] }), update => update({ state }));
    const request = { id: 'job-1', path: '/tmp/a.gcode.3mf', name: 'café & #1.gcode.3mf',
        deviceId: 'desk', plateIndex: 2, useAms: true,
        options: { timelapse: false, bedLeveling: true, flowCali: false },
        mappings: [{ filamentId: 1, amsId: 0, slotId: 2, external: false }] };
    return { adapter, request, state, store, modal, context,
        counts: () => ({ sends, imports, navigations, nativeFrames, nativeCancellations }) };
}

test('supplies cancellable frames only during a hidden handoff', async () => {
    const f = await fixture();
    const w = f.context.window;
    assert.equal(w.requestAnimationFrame(() => {}), 1);
    w.cancelAnimationFrame(1);
    assert.equal(f.counts().nativeCancellations, 1);
    f.adapter.beginBackground();
    let cancelledRan = false;
    const cancelled = w.requestAnimationFrame(() => { cancelledRan = true; });
    assert.ok(cancelled < 0);
    w.cancelAnimationFrame(cancelled);
    const timestamp = await new Promise(resolve => w.requestAnimationFrame(resolve));
    assert.equal(typeof timestamp, 'number');
    assert.equal(cancelledRan, false);
    assert.equal(f.counts().nativeFrames, 1);
    f.context.document.visibilityState = 'visible';
    assert.equal(w.requestAnimationFrame(() => {}), 2);
    f.context.document.visibilityState = 'hidden';
    f.adapter.endBackground();
    assert.equal(w.requestAnimationFrame(() => {}), 3);
});

test('prepares the exact plate, printer, options and AMS slot without sending', async () => {
    const f = await fixture();
    const result = await f.adapter.prepare(f.request);
    assert.equal(result.needsReview, false);
    assert.equal(result.dialogReady, true);
    assert.equal(result.deviceId, 'desk'); assert.equal(result.plateIndex, 2);
    assert.equal(result.options.bedLeveling, true); assert.equal(result.options.flowCali, false);
    assert.equal(f.state.filamentTrayMappings[0].apiTray.matchedTray.slotId, '2');
    assert.equal(f.counts().sends, 0);
    f.state.deviceId = 'desk';
    f.state.filamentTrayMappings[0].apiTray.matchedTray = { type: 'ams', amsId: '0', slotId: '1' };
    assert.equal((await f.adapter.submit(f.request.id)).status, 'attention');
    assert.equal(f.counts().sends, 0);
});
test('unknown printer, absent mapping and normalized options require review', async () => {
    for (const kind of ['printer', 'mapping', 'options']) {
        const f = await fixture({ normalize: kind === 'options' });
        if (kind === 'printer') f.request.deviceId = 'missing';
        if (kind === 'mapping') f.request.mappings[0].slotId = 99;
        if (kind === 'options') f.request.options.timelapse = true;
        assert.equal((await f.adapter.prepare(f.request)).needsReview, true);
        assert.equal((await f.adapter.submit(f.request.id)).status, 'attention');
        assert.equal(f.counts().sends, 0);
    }
});
test('recovers the asynchronous startup redirect before presenting the send dialog', async () => {
    const f = await fixture({ startupRedirect: true });
    assert.equal((await f.adapter.prepare(f.request)).dialogReady, true);
    assert.equal(f.counts().navigations, 2);
    assert.equal(f.counts().sends, 0);
});
test('honors Connect disabled state and rechecks changed selections before Send', async () => {
    const f = await fixture({ disabled: true }); await f.adapter.prepare(f.request);
    assert.equal((await f.adapter.submit(f.request.id)).status, 'attention');
    f.modal.okButtonProps.disabled = false; f.state.deviceId = 'other';
    assert.equal((await f.adapter.submit(f.request.id)).status, 'attention');
    assert.equal(f.counts().sends, 0);
});
test('invokes the existing Send handler once and rejects duplicate submission', async () => {
    const f = await fixture(); await f.adapter.prepare(f.request);
    assert.equal((await f.adapter.submit(f.request.id)).status, 'submitted');
    await assert.rejects(f.adapter.submit(f.request.id), /already submitted/);
    await assert.rejects(f.adapter.prepare(f.request), /already submitted/);
    assert.equal(f.counts().sends, 1);
});
test('warning flow returns attention and does not confirm or retry', async () => {
    const f = await fixture({ warning: true }); await f.adapter.prepare(f.request);
    assert.equal((await f.adapter.submit(f.request.id)).status, 'attention');
    assert.equal(f.counts().sends, 1);
    await assert.rejects(f.adapter.submit(f.request.id), /already submitted/);
});
test('closing the send dialog alone is not reported as a successful print', async () => {
    const f = await fixture({ cancelled: true }); await f.adapter.prepare(f.request);
    assert.equal((await f.adapter.submit(f.request.id)).status, 'attention');
    assert.equal(f.counts().sends, 1);
});
test('rejects missing plates, bad imports, and concurrent sends before dispatch', async () => {
    const f = await fixture(); f.request.plateIndex = 9;
    await assert.rejects(f.adapter.prepare(f.request), /selected plate is missing/);
    f.state.sending = true;
    await assert.rejects(f.adapter.prepare(f.request), /already sending/);
    const broken = await fixture({ importFails: true });
    await assert.rejects(broken.adapter.prepare(broken.request), /could not import/);
    assert.equal(f.counts().sends + broken.counts().sends, 0);
});

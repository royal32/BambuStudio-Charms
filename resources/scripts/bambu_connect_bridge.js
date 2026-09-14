// Adapter for Connect 2.5.0-beta.15. Evaluated as a factory in its own renderer.
// Keep imports and store symbols in sync with BambuConnectBridge.mm when updating.
(function (getPrint, getDevices, setPrintState, getAuth) {
    let requestId;
    let submitted = false;
    let needsReview = false;
    let expected;
    const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
    let backgroundFrames = false;
    let frameSequence = 0;
    const frames = new Map();
    const nativeFrame = window.requestAnimationFrame?.bind(window);
    const nativeCancelFrame = window.cancelAnimationFrame?.bind(window);
    // Chromium suspends animation frames in hidden windows even with background
    // timer throttling disabled. Keep routing and UI updates moving during a
    // Studio handoff; idle behavior stays native.
    if (nativeFrame && nativeCancelFrame) {
        window.requestAnimationFrame = callback => {
            if (!backgroundFrames || document.visibilityState !== 'hidden') return nativeFrame(callback);
            const id = --frameSequence;
            frames.set(id, setTimeout(() => { frames.delete(id); callback(performance.now()); }, 16));
            return id;
        };
        window.cancelAnimationFrame = id => {
            if (id < 0) { clearTimeout(frames.get(id)); frames.delete(id); }
            else nativeCancelFrame(id);
        };
    }
    function mappingMatches(mapping) {
        const intended = expected.mappings?.find(m => String(m.filamentId) === String(mapping.sliceConfigFilament.id));
        const actual = mapping.apiTray?.matchedTray;
        return intended && actual && (intended.external
            ? actual.type === 'external' && Number(actual.extruderId) === intended.extruderId
            : actual.type === 'ams' && Number(actual.amsId) === intended.amsId && Number(actual.slotId) === intended.slotId);
    }
    function intentMatches() {
        const state = getPrint().state;
        return (!expected.accountUserId || String(getAuth?.().user?.uid ?? '') === expected.accountUserId) &&
            (!expected.deviceId || state.deviceId === expected.deviceId) &&
            (expected.plateIndex === undefined || Number(state.activePlate?.sliceInfoConfig.index) === expected.plateIndex) &&
            Object.entries(expected.options ?? {}).every(([key, value]) => state.printOptions[key] === value) &&
            (expected.useAms === undefined || state.useAms === expected.useAms) &&
            (!expected.mappings || state.filamentTrayMappings.every(mappingMatches));
    }

    function sendModal() {
        // Use Connect's current modal props and Send handler, including its disabled
        // predicate and warning flow. Never call the lower-level _print directly.
        const root = document.getElementById('root');
        const containerKey = root && Object.keys(root).find(k => k.startsWith('__reactContainer$'));
        const element = root?.firstElementChild;
        const key = element && Object.keys(element).find(k => k.startsWith('__reactFiber$'));
        let fiber = containerKey ? root[containerKey] : key && element[key];
        if (!fiber) return;
        while (fiber.return) fiber = fiber.return;
        fiber = fiber.stateNode?.current ?? fiber;
        const stack = [fiber];
        const seen = new Set();
        while (stack.length) {
            const current = stack.pop();
            if (!current || seen.has(current)) continue;
            seen.add(current);
            const props = current.memoizedProps;
            if (props?.visible && props.okButtonProps && typeof props.onOk === 'function' &&
                props.title === window.__studioConnectSendTitle) return props;
            if (current.child) stack.push(current.child);
            if (current.sibling) stack.push(current.sibling);
        }
    }

    return {
        beginBackground() { backgroundFrames = true; },
        endBackground() { backgroundFrames = false; },
        async prepare(request) {
            if (request.accountUserId && String(getAuth?.().user?.uid ?? '') !== request.accountUserId)
                throw Error('The Connect account changed. Reopen this job from Studio.');
            if (getPrint().state.sending) throw Error('Connect is already sending a print job.');
            if (requestId === request.id && submitted) throw Error('This job was already submitted.');
            requestId = undefined;
            submitted = false;
            needsReview = false;
            expected = request;
            getPrint().setModalSource(undefined);
            getPrint().closeFile();
            const io = await import('./assets/index-B_vXgx8V.js');
            window.__studioConnectSendTitle = io.Y('Send to print');
            const { file } = await io.a2.invoke('readFile', { path: request.path });
            const result = await getPrint().importFile(new File(
                [new Uint8Array(file.data ?? file)], request.name, { type: '3mf' }));
            if (!result.success) throw Error('Connect could not import the sliced job.');
            // Mount the print page before opening its modal. Navigating after
            // opening it can run the old route's modal cleanup and dismiss it.
            await (await import('./assets/router-DcHfLi1L.js')).router.navigate({ to: '/print' });
            await delay(50);

            const store = getPrint();
            if (request.plateIndex !== undefined) {
                const plate = store.state.gcode3MF.allGcodeData.find(
                    p => Number(p.sliceInfoConfig.index) === request.plateIndex);
                if (!plate) throw Error('The selected plate is missing from the imported job.');
                store.setActivePlate(plate);
            }
            const device = request.deviceId && getDevices().devices.find(d => d.id === request.deviceId);
            if (request.deviceId && !device) needsReview = true;
            store.openPrintModal('print', device);
            if (request.options) store.setPrintOptions(request.options);
            if (typeof request.useAms === 'boolean') setPrintState(s => { s.state.useAms = request.useAms; });

            // Studio's mapping IDs are zero-based; sliced filament IDs are one-based.
            if (request.mappings && device) {
                const currentTrays = () => {
                    const current = getDevices().devices.find(d => d.id === request.deviceId);
                    return [...(current?.amsFilament?.amsItems ?? []).flatMap(ams => ams.trays ?? []),
                        ...(current?.amsFilament?.externalTrays ?? [])];
                };
                // Printer reports arrive after the account's device list. Read the
                // current store, not the stale device snapshot from before import.
                for (const deadline = Date.now() + 5000; Date.now() < deadline && currentTrays().length === 0;) await delay(50);
                const trays = currentTrays();
                for (const mapping of request.mappings) {
                    const tray = trays.find(t => mapping.external
                        ? t.type === 'external' && Number(t.extruderId) === mapping.extruderId
                        : t.type === 'ams' && Number(t.amsId) === mapping.amsId && Number(t.slotId) === mapping.slotId);
                    if (!tray) { needsReview = true; continue; }
                    store.setFilamentTrayMapping(String(mapping.filamentId), tray);
                }
            }
            if (request.requireReview) needsReview = true;
            // Connect's asynchronous sign-in can redirect to Devices after its
            // initial shell has rendered. Reassert this requested route until the
            // actual send modal has remained mounted across several updates.
            const router = (await import('./assets/router-DcHfLi1L.js')).router;
            let stable = 0;
            for (const deadline = Date.now() + 10000; Date.now() < deadline && stable < 3;) {
                if (getPrint().modalSource !== 'print') throw Error('The print dialog was closed in Connect.');
                if (!window.location.hash.startsWith('#/print')) {
                    stable = 0;
                    await router.navigate({ to: '/print' });
                } else if (sendModal()) ++stable;
                else stable = 0;
                await delay(100);
            }
            if (!sendModal()) throw Error('Connect did not open its print dialog (page ' + window.location.hash +
                ', modal ' + getPrint().modalSource + ', printers ' + getDevices().devices.length + ').');
            await delay(150); // Let Connect's capability effects normalize options.
            if (!intentMatches()) needsReview = true;
            requestId = request.id;
            return { status: 'ready', needsReview, dialogReady: !!sendModal(), deviceId: getPrint().state.deviceId,
                plateIndex: Number(getPrint().state.activePlate.sliceInfoConfig.index),
                options: getPrint().state.printOptions, useAms: getPrint().state.useAms,
                mappings: getPrint().state.filamentTrayMappings.map(m => ({
                    filamentId: m.sliceConfigFilament.id, type: m.apiTray?.matchedTray?.type,
                    extruderId: m.apiTray?.matchedTray.extruderId,
                    amsId: m.apiTray?.matchedTray.amsId, slotId: m.apiTray?.matchedTray.slotId })) };
        },
        async submit(id) {
            if (id !== requestId || submitted) throw Error('The prepared job is stale or already submitted.');
            const modal = sendModal();
            if (needsReview || !intentMatches() || !modal || modal.okButtonProps.disabled || modal.okButtonProps.loading)
                return { status: 'attention' };
            submitted = true; // Set before dispatch; uncertain outcomes must never retry.
            await modal.onOk();
            await delay(100);
            return this.status();
        },
        status() {
            if (getPrint().state.sending) return { status: 'sending' };
            const devicePage = '#/devices/' + encodeURIComponent(expected?.deviceId ?? getPrint().state.deviceId);
            if (submitted && getPrint().modalSource === undefined && window.location.hash.startsWith(devicePage))
                return { status: 'submitted' };
            return { status: submitted ? 'attention' : 'ready' };
        }
    };
})

// Run: node --test tests/sliced_library.test.cjs
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../resources/web/homepage3/js/sliced-library.js'), 'utf8');

function fixture() {
    class Element {
        constructor(tag) { this.tag = tag; this.children = []; this.events = {}; this.attributes = {}; }
        set textContent(value) { this.text = value; this.children = []; }
        get textContent() { return this.text || ''; }
        set innerHTML(_) { throw new Error('Library filenames must be rendered as text, never HTML'); }
        appendChild(child) { this.children.push(child); }
        addEventListener(name, callback) { this.events[name] = callback; }
        setAttribute(name, value) { this.attributes[name] = value; }
        querySelectorAll(tag) { return this.children.flatMap(child => [...(child.tag === tag ? [child] : []), ...child.querySelectorAll(tag)]); }
    }
    const elements = new Map();
    const el = name => {
        if (!elements.has(name)) elements.set(name, new Element('div'));
        return elements.get(name);
    };
    const messages = [];
    const context = vm.createContext({ window: {}, document: {
        getElementById: name => el(name.replace('SlicedLibrary', '')),
        createElement: tag => new Element(tag)
    }, setInterval() {}, SendWXMessage: message => messages.push(JSON.parse(message)) });
    vm.runInContext(source, context);
    const library = context.window.SlicedLibrary;
    library.init();
    const list = (entries, token = 'first') => library.handle({ command: 'sliced_library_list', entries,
        token, folder: '/library', scanning: false, error: '' });
    const search = query => { el('Search').value = query; el('Search').events.input.call(el('Search')); };
    return { library, list, search, el, messages };
}

test('search includes every indexed file and folder without a native scan per keystroke', () => {
    const f = fixture();
    f.list(Array.from({ length: 5000 }, (_, i) => ({ id: `${i}.gcode.3mf`, name: `Glasses ${i}`, folder: 'Holiday/Red' })));
    assert.equal(f.el('Grid').children.length, 60);
    const before = f.messages.length;
    f.search('hOLIday 4999');
    assert.equal(f.el('Grid').children.length, 1);
    assert.equal(f.el('Count').textContent, '1 of 5000 files');
    assert.ok(f.messages.slice(before).every(message => message.command === 'sliced_library_thumbnails'));
    f.search('not present');
    assert.equal(f.el('Grid').children.length, 0);
    assert.equal(f.el('Status').textContent, 'No plates match your search.');
});

test('Unicode and markup in filenames remain text and round-trip unchanged to Print', () => {
    const f = fixture();
    const id = 'Mamá’s "<glasses>".gcode.3mf';
    f.list([{ id, name: id, folder: 'Red & white' }]);
    const name = f.el('Grid').children[0].children[1].children[0];
    assert.equal(name.textContent, id);
    const button = f.el('Grid').querySelectorAll('button')[0];
    button.onclick();
    button.onclick();
    const prints = f.messages.filter(message => message.command === 'sliced_library_print');
    assert.equal(prints.length, 1);
    assert.equal(prints[0].id, id);
    assert.equal(prints[0].token, 'first');
    f.library.handle({ command: 'sliced_library_opened' });
    assert.equal(f.el('Grid').querySelectorAll('button')[0].disabled, false);
});

test('a folder change clears old jobs and rejects thumbnails from the previous scan', () => {
    const f = fixture();
    const entries = [{ id: 'same.gcode.3mf', name: 'Same', folder: '' }];
    f.list(entries);
    f.library.handle({ command: 'sliced_library_status', folder: '/other', scanning: true });
    assert.equal(f.el('Grid').children.length, 0);
    f.list(entries, 'second');
    f.library.handle({ command: 'sliced_library_thumbnails', token: 'first', entries: [{ id: entries[0].id, image: 'stale' }] });
    assert.equal(f.el('Grid').querySelectorAll('img').length, 0);
    f.library.handle({ command: 'sliced_library_thumbnails', token: 'second', entries: [{ id: entries[0].id, image: 'current' }] });
    assert.equal(f.el('Grid').querySelectorAll('img')[0].src, 'current');
});

test('refresh retains unchanged thumbnails and invalidates replaced files', () => {
    const f = fixture();
    const entry = { id: 'job.gcode.3mf', name: 'Job', folder: '', stamp: 'old' };
    f.list([entry]);
    f.library.handle({ command: 'sliced_library_thumbnails', token: 'first', entries: [{ id: entry.id, image: 'cached' }] });
    f.list([entry], 'refresh');
    assert.equal(f.el('Grid').querySelectorAll('img')[0].src, 'cached');
    f.list([{ ...entry, stamp: 'changed' }], 'replacement');
    assert.equal(f.el('Grid').querySelectorAll('img').length, 0);
});

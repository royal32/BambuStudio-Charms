// No account access: exercise homepage messages and the collapsed-menu structure.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.join(__dirname, '../resources/web/homepage3');

test('account actions and upstream access checks share the homepage bridge', () => {
    const sent = [];
    const context = vm.createContext({ window: {}, SendWXMessage: value => sent.push(JSON.parse(value)) });
    vm.runInContext(fs.readFileSync(path.join(root, 'js/left.js'), 'utf8'), context);
    context.SendMsg_GetAccountProfiles();
    context.SendMsg_GetMenuAccess();
    context.OnSwitchAccount('synthetic-profile');
    let stopped = false;
    context.OnAddAccount({ stopPropagation() { stopped = true; } });
    assert.deepEqual(sent.map(value => value.command), [
        'get_account_profiles', 'homepage_leftmenu_access_get', 'homepage_switch_account', 'homepage_add_account'
    ]);
    assert.equal(sent[2].profile_id, 'synthetic-profile');
    assert.equal(stopped, true);
});

test('collapsed sidebar keeps one visible account menu with all profile actions', () => {
    const html = fs.readFileSync(path.join(root, 'left.html'), 'utf8');
    const stack = [];
    const ancestry = new Map();
    for (const tag of html.matchAll(/<\/?div\b[^>]*>/gi)) {
        if (tag[0].startsWith('</')) { stack.pop(); continue; }
        const id = /\bid="([^"]+)"/.exec(tag[0])?.[1];
        if (id) {
            assert.equal(ancestry.has(id), false, `duplicate ${id}`);
            ancestry.set(id, [...stack]);
        }
        stack.push(tag[0]);
    }
    const parents = ancestry.get('DropdownWrapper');
    assert.ok(parents.some(tag => tag.includes('id="Login2"')));
    assert.ok(parents.every(tag => !/\bPullBtn\b/.test(tag)));
    for (const id of ['AccountList', 'AddAccountFooter', 'SignInFooter', 'LogoutFooter'])
        assert.ok(ancestry.get(id).some(tag => tag.includes('id="DropdownWrapper"')));
});

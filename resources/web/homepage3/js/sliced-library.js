// Search runs entirely against the local index; native I/O is never per keystroke.
window.SlicedLibrary = (function () {
    var entries = [], images = new Map(), cards = new Map(), token = '', folder = '';
    var limit = 60, busy = false, scanning = false, error = '', query = '';
    function el(id) { return document.getElementById('SlicedLibrary' + id); }
    function send(command, data) {
        SendWXMessage(JSON.stringify(Object.assign({ command: 'sliced_library_' + command, sequence_id: Date.now() }, data || {})));
    }
    function requestImages() {
        var ids = Array.from(cards.keys()).filter(function (id) { return !images.has(id); }).slice(0, 60);
        if (ids.length && token && !scanning) send('thumbnails', { token: token, ids: ids });
    }
    function showImage(id, image) {
        var preview = cards.get(id);
        if (!preview) return;
        preview.textContent = '';
        if (image) {
            var img = document.createElement('img');
            img.src = image;
            img.alt = '';
            preview.appendChild(img);
        } else {
            var placeholder = document.createElement('span');
            placeholder.className = 'SlicedLibraryPlaceholder';
            placeholder.textContent = 'Sliced plate';
            preview.appendChild(placeholder);
        }
    }
    function render() {
        var words = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
        var matches = entries.filter(function (entry) { return words.every(function (word) { return entry.search.indexOf(word) !== -1; }); });
        var grid = el('Grid');
        grid.textContent = '';
        cards.clear();
        el('Count').textContent = matches.length + (words.length ? ' of ' + entries.length : '') + (entries.length === 1 ? ' file' : ' files');
        el('Status').textContent = error || (scanning ? 'Checking your folder…' : !folder ?
            'Choose a folder containing .gcode.3mf files to get started.' : !entries.length ?
            'No sliced plates yet. Export sliced plates individually into this folder, then refresh.' : !matches.length ?
            'No plates match your search.' : '');
        el('Status').hidden = !el('Status').textContent;
        el('Folder').textContent = folder;
        el('Folder').title = folder;
        el('Refresh').disabled = scanning || !folder;
        matches.slice(0, limit).forEach(function (entry) {
            var card = document.createElement('article');
            card.className = 'SlicedLibraryCard';
            var preview = document.createElement('div');
            preview.className = 'SlicedLibraryPreview';
            card.appendChild(preview);
            cards.set(entry.id, preview);
            showImage(entry.id, images.get(entry.id));
            var details = document.createElement('div');
            details.className = 'SlicedLibraryDetails';
            var name = document.createElement('div');
            name.className = 'SlicedLibraryName TextS1';
            name.textContent = entry.name;
            name.title = entry.name;
            var subfolder = document.createElement('div');
            subfolder.className = 'SlicedLibrarySubfolder TextS2';
            subfolder.textContent = entry.folder || '.gcode.3mf';
            subfolder.title = entry.id;
            var print = document.createElement('button');
            print.type = 'button';
            print.className = 'SlicedLibraryPrint';
            print.textContent = 'Print…';
            print.setAttribute('aria-label', 'Print ' + entry.name);
            print.disabled = busy || scanning;
            print.onclick = function () {
                if (busy || scanning) return;
                busy = true;
                grid.querySelectorAll('button').forEach(function (button) { button.disabled = true; });
                print.textContent = 'Opening…';
                send('print', { token: token, id: entry.id });
            };
            details.appendChild(name);
            details.appendChild(subfolder);
            details.appendChild(print);
            card.appendChild(details);
            grid.appendChild(card);
        });
        el('More').hidden = matches.length <= limit;
        requestImages();
    }
    return {
        init: function () {
            if (!el('Grid')) return;
            el('Search').addEventListener('input', function () { query = this.value; limit = 60; render(); });
            send('get');
            // Thumbnails load only for visible results, separately from the fast index.
            setInterval(requestImages, 1200);
            setInterval(function () { if (!busy) send('get'); }, 30000);
        },
        choose: function () { send('choose'); },
        refresh: function () { send('refresh'); },
        more: function () { limit += 60; render(); },
        handle: function (message) {
            if (typeof message.command !== 'string' || message.command.indexOf('sliced_library_') !== 0) return false;
            if (!el('Grid')) return true;
            if (message.command === 'sliced_library_status') {
                if (folder !== message.folder) { entries = []; images.clear(); }
                folder = message.folder;
                scanning = message.scanning;
                error = '';
                render();
            } else if (message.command === 'sliced_library_list') {
                if (folder !== message.folder) images.clear();
                else if (token !== message.token) {
                    var previous = new Map(entries.map(function (entry) { return [entry.id, entry.stamp]; }));
                    var unchanged = new Set(message.entries.filter(function (entry) {
                        return previous.has(entry.id) && previous.get(entry.id) === entry.stamp;
                    }).map(function (entry) { return entry.id; }));
                    images.forEach(function (_, id) { if (!unchanged.has(id)) images.delete(id); });
                }
                token = message.token;
                folder = message.folder;
                scanning = message.scanning;
                error = message.error;
                entries = message.entries.map(function (entry) {
                    entry.search = (entry.name + ' ' + entry.folder).toLocaleLowerCase();
                    return entry;
                });
                render();
            } else if (message.command === 'sliced_library_thumbnails' && message.token === token) {
                message.entries.forEach(function (entry) {
                    images.set(entry.id, entry.image);
                    showImage(entry.id, entry.image);
                });
                requestImages();
            } else if (message.command === 'sliced_library_opened') {
                busy = false;
                render();
            }
            return true;
        }
    };
})();

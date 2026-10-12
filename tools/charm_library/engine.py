"""Persistent planning, resumable publication and provenance for the library."""
from contextlib import contextmanager
import copy
import fcntl
import gc
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import time
import uuid
import xml.etree.ElementTree as ET
import zipfile

from . import artwork, geometry, package, prepare, projects, shared

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 1
# Flat charms can include 15.5 mm indicators/backs. Taller plates need review.
MAX_VECTOR_HEIGHT_MM = 20


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def signature(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.tmp')
    with temp.open('w') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    temp.replace(path)


def default_config(home):
    candidates = sorted((Path.home() / 'Library/Application Support/BambuStudio-Accounts/profiles').glob('*'))
    profile = next((p for p in candidates if (p / 'user').exists()), Path.home() / 'Library/Application Support/BambuStudio')
    return {'version': 1, 'sources': [str(home / 'inbox')],
            'library': str(Path.home() / 'Desktop/Sliced_Temp'),
            'slicer': '/Applications/BambuStudio.app/Contents/MacOS/BambuStudio',
            'profile_root': str(profile), 'presets': {
                'machine': 'Bambu Lab A1 mini 0.4 nozzle - 1.1.0',
                'process': '0.20mm Charms @BBL A1M 1.0.3',
                'filament': 'Atomic PLA  A1 Mini 1.2'}, 'plate_names': {}, 'editable_projects': True}


class Library:
    def __init__(self, home=None):
        self.home = Path(home or os.environ.get('CHARM_LIBRARY_HOME', ROOT / '.charm-library')).expanduser().resolve()
        self.config_path = self.home / 'config.json'
        self.state_path = self.home / 'manifest.json'
        self.journal = self.home / 'publication-pending.json'
        self.config = json.loads(self.config_path.read_text()) if self.config_path.exists() else default_config(self.home)
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {'version': SCHEMA, 'projects': {}}
        if self.state.get('version') != SCHEMA or self.config.get('version') != SCHEMA:
            raise ValueError('Unsupported library state version; keep the existing state for migration.')

    @contextmanager
    def locked(self):
        self.home.mkdir(parents=True, exist_ok=True)
        with (self.home / 'operation.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Another library operation is running. Wait for it to finish.') from None
            # Reload after acquiring the lock so an already-created object cannot
            # overwrite another run's manifest or settings.
            self.__init__(self.home)
            if self.journal.exists():
                print('Resuming an interrupted publication…', flush=True)
                self.recover()
            yield

    def initialize(self):
        self.home.mkdir(parents=True, exist_ok=True)
        (self.home / 'inbox').mkdir(exist_ok=True)
        if not self.config_path.exists():
            atomic_json(self.config_path, self.config)
        if not self.state_path.exists():
            atomic_json(self.state_path, self.state)

    def save_config(self):
        atomic_json(self.config_path, self.config)

    def recipes(self):
        presets = prepare.resolve_presets(self.config)
        binary = Path(self.config['slicer']).expanduser().resolve()
        if not binary.is_file():
            raise ValueError('Slicer executable is missing. Set Settings → Configure.')
        recipe = signature({'presets': presets, 'binary': digest(binary), 'version': 1,
                            'implementation': [digest(Path(m.__file__)) for m in (geometry, prepare, package)],
                            'library': str(Path(self.config['library']).expanduser().resolve())})
        preview_recipe = signature([digest(ROOT / 'tools/generate_plate_vector_preview.py'), digest(Path(artwork.__file__))])
        return recipe, preview_recipe, presets

    def files(self):
        result = {}
        for key, project in self.state['projects'].items():
            for path, sha in project['outputs'].items():
                if path in result and result[path]['sha256'] != sha:
                    raise ValueError('Manifest has conflicting output owners: ' + path)
                item = result.setdefault(path, {'sha256': sha, 'owners': set()})
                item['owners'].add(key)
        return result

    def shared_root(self):
        return Path(self.config['library']).expanduser().resolve()

    def excluded_output(self, path, project=None, records=None):
        records = shared.exclusions(self.shared_root()) if records is None else records
        projects = [project] if project else self.state['projects'].values()
        return any(shared.output_excluded(self.shared_root(), p, path, records) for p in projects)

    def discover(self):
        found = {}
        roots = [Path(p).expanduser().resolve() for p in self.config['sources']]
        for root in roots:
            if not root.exists():
                print('Source unavailable (retained existing records): ' + str(root), flush=True)
                continue
            paths = [root] if root.is_file() else sorted(root.rglob('*'))
            for path in paths:
                if not path.is_file() or path.suffix.lower() != '.3mf' or path.name.lower().endswith('.gcode.3mf'):
                    continue
                if any(part.startswith('.') for part in path.relative_to(root.parent if root.is_file() else root).parts):
                    continue
                path = path.resolve()
                # A broad source folder must never ingest our own exports/work.
                if path.is_relative_to(Path(self.config['library']).expanduser().resolve()):
                    continue
                if path.is_relative_to(self.home / 'work') or path.is_relative_to(self.home / 'archive'):
                    continue
                found[str(path)] = path
        # Imported and individually registered projects remain tracked even when
        # their original folder is removed from discovery. Missing inputs are
        # shown, never interpreted as authorization to delete print files.
        for project in self.state['projects'].values():
            source = projects.source_path(self, project)
            found.setdefault(str(source), source)
        retired = {p for project in self.state['projects'].values() for p in project.get('retired_sources', [])}
        return sorted(p for name, p in found.items() if name not in retired)

    def plan(self, project_filter='', force=False, previews_only=False):
        recipe, preview_recipe, presets = self.recipes()
        exclusions = shared.exclusions(self.shared_root())
        previous = {str(projects.source_path(self, p)): p for p in self.state['projects'].values()}
        rows, hashes = [], {}
        seen = {p['source_sha256']: p['name'] for p in previous.values()
                if projects.source_path(self, p).is_file() and digest(projects.source_path(self, p)) == p['source_sha256']}
        for path in self.discover():
            old = previous.get(str(path))
            key = old['id'] if old else signature(str(path))[:16]
            name = old['name'] if old else path.stem
            if project_filter and project_filter.casefold() not in (name + ' ' + key).casefold():
                continue
            category = old.get('category', 'Bogg Charm') if old else 'Bogg Charm'
            for source_root, source_category in sorted(self.config.get('source_categories', {}).items(), key=lambda p: len(p[0])):
                if path.is_relative_to(Path(source_root)):
                    category = source_category
            row = {'id': key, 'name': name, 'source': str(path), 'old': old, 'category': category}
            if not path.is_file():
                row.update(action='unavailable', reason='Source file is missing; outputs retained')
                rows.append(row)
                continue
            source_sha = digest(path)
            current_recipe = signature([recipe, self.config['plate_names'].get(key, {}), category])
            row.update(source_sha256=source_sha, recipe=current_recipe, preview_recipe=preview_recipe)
            damaged = []
            if old:
                for output, sha in old['outputs'].items():
                    if self.excluded_output(output, old, exclusions):
                        continue
                    if output not in hashes:
                        hashes[output] = digest(output) if Path(output).is_file() else None
                    if hashes[output] != sha:
                        damaged.append(output)
            if damaged and any(hashes[p] is not None for p in damaged):
                # Missing managed files can be rebuilt; edited files need review.
                edited = [p for p in damaged if hashes[p] is not None]
                action, reason = ('conflict', 'Managed output was edited: ' + edited[0]) if edited else ('build', 'Managed output is missing')
            elif not old and source_sha in seen:
                action, reason = 'duplicate', 'Identical source already discovered: ' + seen[source_sha]
            elif not old:
                action, reason = 'build', 'New project'
            elif source_sha != old['source_sha256']:
                action, reason = 'build', 'Source changed'
            elif current_recipe != old['recipe']:
                action, reason = 'build', 'Profiles, slicer, names or preparation changed'
            elif any(p.get('excluded', False) and not shared.plate_excluded(old, p, exclusions) for p in old['plates']):
                action, reason = 'build', 'Previously excluded plate was re-included'
            elif damaged:
                action = 'build'
                reason = 'Managed output is missing'
            elif force or preview_recipe != old.get('preview_recipe'):
                action, reason = ('preview', 'Preview update requested') if previews_only or not force else ('build', 'Rebuild requested')
            else:
                action, reason = 'skip', 'Unchanged and verified'
            if previews_only and action == 'build':
                action, reason = 'blocked', 'Slice/update the project before regenerating its previews'
            if old and (action == 'preview'):
                prepared = Path(old['prepared_input'])
                if not prepared.is_file() or digest(prepared) != old['prepared_sha256']:
                    action, reason = 'conflict', 'Prepared source is missing or changed'
            seen.setdefault(source_sha, name)
            row.update(action=action, reason=reason)
            rows.append(row)
        return rows, presets

    def recover(self):
        transaction = json.loads(self.journal.read_text())
        def excluded(action):
            records = shared.exclusions(self.shared_root())
            return any(shared.output_excluded(self.shared_root(), p, action['target'], records)
                       for p in transaction['state']['projects'].values())
        # Check every action before continuing a partially completed operation.
        for action in transaction['actions']:
            if excluded(action):
                continue
            path = Path(action['target'])
            current = digest(path) if path.is_file() else None
            if current not in (action['before'], action['after']):
                raise ValueError('Publication conflict; preserved journal and backups: ' + str(path))
            if action['source'] and digest(action['source']) != action['after']:
                raise ValueError('Staged output changed; preserved publication journal')
        for action in transaction['actions']:
            if excluded(action):
                continue
            path = Path(action['target'])
            current = digest(path) if path.is_file() else None
            if current == action['after']:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.exists():
                backup = Path(action['backup'])
                backup.parent.mkdir(parents=True, exist_ok=True)
                if not backup.exists():
                    shutil.copy2(path, backup)
            if action['source']:
                temp = path.with_name('.' + path.name + '.publishing')
                shutil.copyfile(action['source'], temp)
                if digest(temp) != action['after']:
                    raise ValueError('Copy verification failed: ' + str(path))
                temp.replace(path)
            else:
                path.unlink()
        atomic_json(self.state_path, transaction['state'])
        self.state = transaction['state']
        self.journal.unlink()

    def publish(self, entry, staged):
        catalog = self.files()
        old = self.state['projects'].get(entry['id'], {})
        records = shared.exclusions(self.shared_root())
        staged = {p: source for p, source in staged.items() if not self.excluded_output(p, entry, records)}
        actions, outputs = [], {}
        for path, sha in old.get('outputs', {}).items():
            if self.excluded_output(path, old, records):
                outputs[path] = sha
        stamp = str(time.time_ns())
        for target, source in staged.items():
            target, source = Path(target), Path(source)
            before = digest(target) if target.is_file() else None
            after = digest(source)
            managed = catalog.get(str(target))
            if before is not None and (not managed or before != managed['sha256']):
                raise ValueError('Refusing to replace an untracked or edited file: ' + str(target))
            if managed and managed['owners'] - {entry['id']} and before != after:
                raise ValueError('Output is shared with another project: ' + str(target))
            outputs[str(target)] = after
            if before != after:
                actions.append(dict(target=str(target), source=str(source), before=before, after=after,
                                    backup=str(self.home / 'archive' / stamp / target.name)))
        for path, sha in old.get('outputs', {}).items():
            if path in outputs or catalog[path]['owners'] - {entry['id']}:
                continue
            current = digest(path) if Path(path).is_file() else None
            if current not in (None, sha):
                raise ValueError('Obsolete output was edited; retained: ' + path)
            if current:
                actions.append(dict(target=path, source=None, before=sha, after=None,
                                    backup=str(self.home / 'archive' / stamp / Path(path).name)))
        entry['outputs'] = outputs
        state = copy.deepcopy(self.state)
        state['projects'][entry['id']] = entry
        atomic_json(self.journal, dict(actions=actions, state=state))
        self.recover()
        shared.sync_projects(self.shared_root(), self.state['projects'])

    def allocate(self, label, key, reserved, old=None):
        library = Path(self.config['library']).expanduser().resolve()
        catalog = self.files()
        candidate = geometry.safe(label) + '.gcode.3mf'
        n = 1
        while True:
            target = library / candidate
            item = catalog.get(str(target))
            available = item['owners'] == {key} if item else not target.exists()
            if candidate.casefold() not in reserved and available:
                reserved.add(candidate.casefold())
                return target
            n += 1
            suffix = ' SB' if label.endswith(' SB') else ''
            stem = label[:-3] if suffix else label
            candidate = geometry.safe(stem) + ' Design ' + str(n) + suffix + '.gcode.3mf'

    def preview(self, prepared, job, plate, policy='auto'):
        if policy != 'auto':
            return None, policy
        spec = importlib.util.spec_from_file_location('plate_artwork', ROOT / 'tools/generate_plate_vector_preview.py')
        exporter = importlib.util.module_from_spec(spec)
        gc.disable()
        try:
            spec.loader.exec_module(exporter)
            data = exporter.generate(Path(prepared), Path(job), plate)
            artwork.validate(data)
            if data['print_sha256'] != artwork.fingerprint(job, plate):
                raise ValueError('Vector print digest mismatch')
            return data, ''
        except (ValueError, KeyError, IndexError, TypeError) as error:
            return None, str(error)
        finally:
            gc.collect()
            gc.enable()

    def process(self, row, presets, previews_only=False):
        folder = self.home / 'work' / row['id'] / uuid.uuid4().hex
        folder.mkdir(parents=True)
        old = row['old']
        if row['action'] == 'preview':
            entry = copy.deepcopy(old)
            prepared = Path(old['prepared_input'])
            plates = [(copy.deepcopy(p), Path(self.config['library']) / p['filename']) for p in old['plates']]
            staged = {}
        else:
            family = old.get('source_family', row['name']) if old else row['name']
            plan = prepare.prepare(Path(row['source']), folder, family, row['category'],
                                   presets, self.config['plate_names'].get(row['id']))
            if digest(row['source']) != row['source_sha256']:
                raise ValueError('Source changed while preparing; retry after saving the project')
            print(f"  Slicing {len(plan['plates'])} plates; log: {folder / 'slice.log'}", flush=True)
            prepare.slice_project(self.config, folder, plan)
            plates = package.export(folder, plan, self.config, presets)
            prepared = folder / 'input.3mf'
            entry = {k: row[k] for k in ('id', 'name', 'source', 'source_sha256', 'recipe')}
            if old:
                entry.update({k: old[k] for k in ('original_source', 'adopted', 'retired_sources', 'editable_project', 'legacy_plates') if k in old})
            entry.update(prepared_input=str(prepared), prepared_sha256=digest(prepared), prepared_project=str(folder / 'prepared.3mf'),
                         source_family=family, category=row['category'], plates=[])
            staged = {}
        reserved = set()
        jobs_by_index = {p['index']: job for p, job in plates}
        previous_policies = {p['index']: p.get('preview_policy', 'auto') for p in old['plates']} if old else {}
        for plate, job in plates:
            # A deletion applies to this plate identity, including its future
            # filename, without pausing the other plates in the source project.
            policy_plate = dict(plate, filename=plate.get('filename', plate['name'] + '.gcode.3mf'))
            excluded = shared.plate_excluded(entry, policy_plate, shared.exclusions(self.shared_root()))
            if row['action'] == 'preview':
                target = Path(self.config['library']).expanduser().resolve() / plate['filename']
                if excluded:
                    continue
            else:
                target = self.allocate(plate['name'], row['id'], reserved)
                staged[str(target)] = str(job)
                plate.update(filename=target.name, preview_policy=previous_policies.get(plate['index'], 'auto'))
                if next(s['dimensions'][2] for s in plan['shifts'] if s['plate'] == plate['index']) > MAX_VECTOR_HEIGHT_MM:
                    plate['preview_policy'] = 'Plate height exceeds 20 mm: review artwork; retain the original thumbnail'
            plate['excluded'] = excluded
            data, reason = (None, '') if excluded else self.preview(prepared, job, plate['index'], plate.get('preview_policy', 'auto'))
            plate['preview_fallback'] = reason
            if not excluded:
                source = folder / ('artwork-' + str(plate['index']) + '.gcode.3mf')
                artwork.write(job, source, data)
                staged[str(target)] = str(source)
            if excluded:
                print('  Excluded plate: ' + plate['name'], flush=True)
            elif reason:
                print('  Original thumbnail: ' + plate['name'] + ' — ' + reason, flush=True)
            if row['action'] != 'preview':
                entry['plates'].append(plate)
            else:
                original = next(p for p in entry['plates'] if p['index'] == plate['index'])
                original.update(preview_fallback=reason)
        for alias in entry.get('legacy_plates', []):
            plate = dict(index=alias['index'], filename=alias['id'])
            if plate['index'] not in jobs_by_index or shared.plate_excluded(entry, plate, shared.exclusions(self.shared_root())):
                continue
            target = shared.child(self.shared_root(), alias['id'])
            canonical = next(p for p in entry['plates'] if p['index'] == plate['index'])
            if row['action'] != 'preview':
                staged[str(target)] = staged[str(self.shared_root() / canonical['filename'])]
            else:
                # An older alias can have different G-code bytes. Generate its
                # own digest-bound preview rather than copying another job's.
                data, _ = self.preview(prepared, target, plate['index'], canonical.get('preview_policy', 'auto'))
                source = folder / ('alias-' + str(plate['index']) + '.gcode.3mf')
                artwork.write(target, source, data)
                staged[str(target)] = str(source)
        if digest(row['source']) != row['source_sha256']:
            raise ValueError('Source changed during processing; staged results were not published')
        entry['preview_recipe'] = row['preview_recipe']
        if row['action'] != 'preview' and self.config.get('editable_projects', True) and not entry.get('editable_project'):
            projects.promote(self, entry, folder / 'prepared.3mf')
            if digest(row['source']) != row['source_sha256']:
                raise ValueError('Source changed while registering its editable project; retry after saving')
        self.publish(entry, staged)

    def update(self, project_filter='', limit=0, force=False, previews_only=False):
        rows, presets = self.plan(project_filter, force, previews_only)
        shared.sync_projects(self.shared_root(), self.state['projects'])
        pending = [r for r in rows if r['action'] in ('build', 'preview')]
        if limit:
            pending = pending[:limit]
        errors, attempted, completed = [], 0, 0
        for index, row in enumerate(pending, 1):
            attempted += 1
            print(f"[{index}/{len(pending)}] {row['name']}: {row['reason']}", flush=True)
            try:
                self.process(row, presets, previews_only)
                completed += 1
            except (OSError, ValueError, KeyError, IndexError, zipfile.BadZipFile, ET.ParseError) as error:
                errors.append({'project': row['name'], 'error': str(error)})
                print('  FAILED: ' + str(error), flush=True)
                if self.journal.exists():
                    break
        for row in rows:
            if row['action'] in ('conflict', 'blocked', 'unavailable'):
                errors.append({'project': row['name'], 'error': row['reason']})
        atomic_json(self.home / 'last-run.json', {'completed': completed, 'attempted': attempted,
                                                'skipped': sum(r['action'] == 'skip' for r in rows), 'errors': errors})
        print(f"Finished: {completed}/{attempted} completed, {sum(r['action'] == 'skip' for r in rows)} unchanged, {len(errors)} errors.")
        return 1 if errors else 0

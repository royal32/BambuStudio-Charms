"""Small folder-relative protocol shared by Studio and the refresh console."""
import json
from pathlib import Path
import uuid
import shutil


def child(root, relative):
    name = Path(relative)
    if not relative or name.is_absolute() or '..' in name.parts or '.' in str(relative).split('/'):
        raise ValueError('Invalid shared library path')
    result = Path(root)
    for part in name.parts:
        result = result / part
        if result.is_symlink():
            raise ValueError('Shared library metadata cannot follow symbolic links: ' + str(result))
    return result


def plate_source_key(plate):
    value = 14695981039346656037
    for byte in plate.encode('utf-8'):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return format(value, '016x')


def index_project(root, project):
    identifier = project['id']
    if not identifier or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in identifier):
        raise ValueError('Invalid source project ID')
    for plate in project['plates']:
        write_json(root, '.charm-library/plate-sources/' + plate_source_key(plate['id']) + '/' + identifier + '.json',
                   {'version': 1, 'plate': plate['id'], 'project_id': identifier})


def read(path):
    if path.stat().st_size > 65536:
        raise ValueError('Shared library record is too large: ' + str(path))
    record = json.loads(path.read_text())
    if not isinstance(record, dict) or record.get('version') != 1:
        raise ValueError('Unsupported shared library record: ' + str(path))
    return record


def exclusions(root):
    removed, restored = [], set()
    directory = child(root, '.charm-library/events')
    if not directory.exists():
        return []
    for path in sorted(directory.glob('*.json')):
        record = read(child(root, str(path.relative_to(root))))
        if record.get('kind') == 'exclude':
            if not isinstance(record.get('id'), str) or not isinstance(record.get('plate'), str) or not record['plate'].lower().endswith('.gcode.3mf'):
                raise ValueError('Invalid shared exclusion: ' + str(path))
            child(root, record['plate'])
            if not isinstance(record.get('targets'), list):
                raise ValueError('Invalid shared exclusion targets: ' + str(path))
            for target in record['targets']:
                if (not isinstance(target, dict) or not isinstance(target.get('project_id'), str)
                        or not isinstance(target.get('plate_index'), int) or target['plate_index'] < 1
                        or not isinstance(target.get('source_sha256'), str)):
                    raise ValueError('Invalid shared exclusion target: ' + str(path))
            removed.append(record)
        elif record.get('kind') == 'restore' and isinstance(record.get('restores'), list) and all(isinstance(r, str) for r in record['restores']):
            restored.update(record['restores'])
        else:
            raise ValueError('Unsupported shared library action: ' + str(path))
    return [r for r in removed if r['id'] not in restored]


def plate_excluded(project, plate, records):
    for record in records:
        if plate['filename'] == record['plate']:
            return True
        for target in record['targets']:
            if (target['project_id'] == project['id'] or target['source_sha256'] == project.get('source_sha256')) and target['plate_index'] == plate['index']:
                return True
    return False


def output_excluded(root, project, path, records):
    try:
        relative = str(Path(path).relative_to(root))
    except ValueError:
        return False
    if any(r['plate'] == relative for r in records):
        return True
    plates = project['plates'] + [dict(index=p['index'], filename=p['id']) for p in project.get('legacy_plates', [])]
    return any(p['filename'] == relative and plate_excluded(project, p, records) for p in plates)


def write_json(root, relative, value):
    from .engine import atomic_json
    path = child(root, relative)
    text = json.dumps(value, indent=2, allow_nan=False) + '\n'
    if path.is_file() and path.read_text() == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    child(root, relative)
    child(root, relative + '.tmp')
    atomic_json(path, value)


def native_projects(root):
    """Studio owns native exports; the old normalizing recipe must not replace them."""
    directory = child(root, '.charm-library/projects')
    if not directory.exists():
        return {}
    result = {}
    for path in directory.glob('*.json'):
        record = read(child(root, path.relative_to(root).as_posix()))
        if record.get('native_export'):
            source = Path(record['source'])
            source = source if source.is_absolute() else child(root, record['source'])
            result[record['id']] = dict(record, resolved_source=str(source.resolve()))
    return result


def sync_projects(root, projects):
    directories = {}

    def actual_id(filename):
        path = child(root, filename)
        if path.parent not in directories:
            directories[path.parent] = {p.name for p in path.parent.iterdir()} if path.parent.exists() else set()
        names = directories[path.parent]
        if path.name not in names and path.is_file():
            # APFS may retain the original export's casing when a managed file
            # is replaced. Cards use the actual directory entry as their ID.
            actual = next((n for n in names if n.casefold() == path.name.casefold()), path.name)
            return (path.parent / actual).relative_to(root).as_posix()
        return filename

    for project in projects.values():
        if not any(Path(p).is_relative_to(root) for p in project['outputs']):
            continue
        registered = child(root, '.charm-library/projects/' + project['id'] + '.json')
        if registered.exists() and read(registered).get('native_export'):
            # Native export updates both shared source and output identities.
            # A stale local CLI manifest must not erase that registration.
            continue
        record = dict(version=1, id=project['id'], name=project['name'], source_sha256=project['source_sha256'],
                      plates=[dict(id=actual_id(p['filename']), index=p['index'], name=p['name']) for p in project['plates']])
        source = Path(project['source'])
        record['source'] = project.get('editable_project') or (source.relative_to(root).as_posix() if source.is_relative_to(root) else str(source))
        record['editable_source'] = bool(project.get('editable_project'))
        record['plates'].extend(project.get('legacy_plates', []))
        write_json(root, '.charm-library/projects/' + project['id'] + '.json', record)
        index_project(root, record)


def restore(library, plate):
    root = Path(library.config['library']).expanduser().resolve()
    records = [r for r in exclusions(root) if r['plate'] == plate or r['id'] == plate]
    if not records:
        raise ValueError('Use a plate filename or exclusion ID from Library → Excluded.')
    # Old exports without a tracked source can be recovered directly. Tracked
    # plates are regenerated by Update so changed models/profiles are honored.
    from .engine import digest
    for record in records:
        tracked = any(plate_excluded(p, q, [record]) for p in library.state['projects'].values() for q in p['plates'])
        if tracked:
            continue
        if not child(root, record['removed_dir'] + '/' + record['plate']).is_file():
            raise ValueError('The recovery copy is unavailable. Allow iCloud to finish syncing before restoring this plate.')
        source = child(root, record['removed_dir'] + '/' + record['plate'])
        target = child(root, record['plate'])
        if target.exists():
            if digest(source) != digest(target):
                raise ValueError('Restoration would overwrite a different file: ' + str(target))
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = child(root, record['plate'] + '.restoring')
        shutil.copyfile(source, temporary)
        if digest(temporary) != digest(source):
            raise ValueError('Restored copy failed verification')
        temporary.replace(target)
    # Re-inclusion is an immutable record, referencing the deletions this user
    # actually saw. It cannot override a concurrent deletion from the other Mac.
    identifier = uuid.uuid4().hex
    write_json(root, '.charm-library/events/' + identifier + '.json',
               dict(version=1, kind='restore', id=identifier, restores=[r['id'] for r in records]))
    print('Plate re-included. Run Library → Update to regenerate it from the current source and profiles.')

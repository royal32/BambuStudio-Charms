#!/usr/bin/env python3
"""Build a portable source lookup from the refresh manifest, without reslicing.

Source records remain authoritative. Index publication is a single directory
rename and refuses incomplete coverage of the current library.
"""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
from charm_library.shared import index_project, read


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--record', type=Path, action='append', default=[], help='Additional authoritative source registration')
    parser.add_argument('--unregistered', action='append', default=[], help='Known plate without a source registration; retain Locate behavior')
    args = parser.parse_args()
    root = args.library.resolve()
    state = json.loads(args.state.read_text())
    records, indexed = [], set()
    for identifier, project in state['projects'].items():
        names = {plate['filename'] for plate in project['plates']}
        names.update(plate['id'] for plate in project.get('legacy_plates', []))
        names.update(Path(name).relative_to(root).as_posix() for name in project.get('outputs', {})
                     if Path(name).is_relative_to(root) and name.endswith('.gcode.3mf'))
        indexed.update(names)
        records.append({'id': identifier, 'plates': [{'id': name} for name in sorted(names)]})
    for path in args.record:
        record = read(path)
        records.append(record)
        indexed.update(plate['id'] for plate in record['plates'])
    live = {p.relative_to(root).as_posix() for p in root.rglob('*.gcode.3mf')
            if not any(part.startswith('.') for part in p.relative_to(root).parts)}
    missing = live - indexed
    print(f'{len(records)} projects; {len(live)} live sliced files; {len(missing)} without source lookup')
    unexpected = missing - set(args.unregistered)
    if unexpected:
        raise SystemExit('Index is incomplete; preserve the current library. Missing: ' + ', '.join(sorted(unexpected)[:20]))
    target = root / '.charm-library/plate-sources'
    if target.exists():
        raise SystemExit('Source index already exists; refusing to replace it.')
    if not args.write:
        return
    parent = root / '.charm-library'
    staging = Path(tempfile.mkdtemp(prefix='source-lookup-', dir=parent))
    try:
        for record in records:
            index_project(staging, record)
        (staging / '.charm-library/plate-sources').rename(target)
    finally:
        shutil.rmtree(staging)
    print('Published source lookup; projects and sliced archives were unchanged.')


if __name__ == '__main__':
    main()

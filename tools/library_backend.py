#!/usr/bin/env python3
"""Operational backend for the Omnitrix library console (also scriptable)."""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

from charm_library.engine import Library, digest, ROOT


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest='action', required=True)
    for name in ('status', 'verify', 'exceptions', 'sources', 'settings', 'doctor', 'setup'):
        commands.add_parser(name)
    for name in ('plan', 'update', 'previews'):
        q = commands.add_parser(name)
        q.add_argument('--project', default='')
        q.add_argument('--limit', type=int, default=0)
        q.add_argument('--force', action='store_true')
    q = commands.add_parser('projects')
    q.add_argument('--project', default='')
    q = commands.add_parser('use-source')
    q.add_argument('--project', required=True)
    q.add_argument('--path', required=True)
    q = commands.add_parser('add-source')
    q.add_argument('--path', required=True)
    q.add_argument('--category', choices=['Bogg Charm', 'Pair Eyewear'], default='Bogg Charm')
    q = commands.add_parser('configure')
    for option in ('library', 'slicer', 'profile-root', 'machine', 'filament', 'process'):
        q.add_argument('--' + option)
    q = commands.add_parser('adopt')
    q.add_argument('--folder', required=True)
    q = commands.add_parser('rename')
    q.add_argument('--project', required=True)
    q.add_argument('--plate', type=int, required=True)
    q.add_argument('--name', required=True)
    return p


def status(library):
    projects = list(library.state['projects'].values())
    jobs = {p['filename'] for project in projects for p in project['plates']}
    exceptions = {p['filename']: p.get('preview_fallback') for project in projects for p in project['plates'] if p.get('preview_fallback')}
    print(f'{len(projects)} tracked projects · {len(jobs)} sliced plates · {len(exceptions)} original-thumbnail fallbacks')
    print('Library: ' + library.config['library'])
    print('Inbox: ' + str(library.home / 'inbox'))
    print('State: ' + str(library.home))
    if library.journal.exists():
        print('Publication interrupted: the next update will resume it before doing new work.')
    return 0


def main(argv=None):
    args = parser().parse_args(argv)
    library = Library()
    if getattr(args, 'limit', 0) < 0:
        raise ValueError('Limit must be zero (all) or positive')
    if args.action == 'status':
        return status(library)
    if args.action == 'settings':
        print(json.dumps(library.config, indent=2))
        return 0
    if args.action == 'projects':
        for project in library.state['projects'].values():
            if args.project.casefold() not in (project['name'] + ' ' + project['id']).casefold():
                continue
            print(f"{project['name']} [{project['id']}]\n  Source: {project['source']}")
            print('  Saved project: ' + project.get('prepared_project', project['prepared_input']))
            for plate in project['plates']:
                print(f"  {plate['index']:3}: {plate['name']}")
        return 0
    if args.action == 'sources':
        for root in library.config['sources']:
            print(('OK   ' if Path(root).exists() else 'OFFLINE ') + root + ' [' + library.config.get('source_categories', {}).get(root, 'Bogg Charm') + ']')
        return 0
    if args.action == 'exceptions':
        seen = set()
        for project in library.state['projects'].values():
            for plate in project['plates']:
                reason = plate.get('preview_fallback')
                if reason and plate['filename'] not in seen:
                    seen.add(plate['filename'])
                    print(plate['name'] + '\n  ' + reason)
        print(f'{len(seen)} tracked preview exceptions. Untracked older exports are left intact.')
        return 0
    if args.action == 'verify':
        errors = []
        files = library.files()
        for path, item in files.items():
            if not Path(path).is_file() or digest(path) != item['sha256']:
                errors.append(path)
        for path in errors:
            print('MISSING OR EDITED: ' + path)
        print(f'{len(files)} managed files checked; {len(errors)} mismatches.')
        return 1 if errors else 0
    if args.action == 'doctor':
        library.recipes()
        import numpy
        import shapely
        print(f'Presets and slicer OK. NumPy {numpy.__version__}; Shapely {shapely.__version__}.')
        print('Output folder: ' + library.config['library'])
        return 0
    if args.action == 'plan':
        rows, _ = library.plan(args.project, args.force)
        for row in rows:
            if row['action'] != 'skip':
                print(f"{row['action'].upper():11} {row['name']} [{row['id']}] — {row['reason']}")
        counts = Counter(r['action'] for r in rows)
        print('Plan: ' + ', '.join(f'{value} {key}' for key, value in sorted(counts.items())))
        print('Dry run only; no files changed.')
        return 1 if any(r['action'] in ('conflict', 'unavailable', 'blocked') for r in rows) else 0
    with library.locked():
        library.initialize()
        if args.action == 'setup':
            runtime = library.home / 'venv'
            if not (runtime / 'bin/python3').exists():
                subprocess.run([sys.executable, '-m', 'venv', str(runtime)], check=True)
            subprocess.run([str(runtime / 'bin/python3'), '-m', 'pip', 'install', '-r', str(ROOT / 'tools/vector-preview-requirements.txt')], check=True)
            print('Runtime ready. Add source folders, review Library → Plan, then run Library → Update.')
        elif args.action == 'add-source':
            path = Path(args.path).expanduser().resolve()
            if not path.exists():
                raise ValueError('Source path does not exist: ' + str(path))
            if path.is_file() and (path.suffix.lower() != '.3mf' or path.name.lower().endswith('.gcode.3mf')):
                raise ValueError('Add an editable .3mf project or a folder containing projects.')
            if str(path) not in library.config['sources']:
                library.config['sources'].append(str(path))
            library.config.setdefault('source_categories', {})[str(path)] = args.category
            library.save_config()
            print('Source registered: ' + str(path))
        elif args.action == 'configure':
            for key in ('library', 'slicer', 'profile_root'):
                if getattr(args, key):
                    library.config[key] = str(Path(getattr(args, key)).expanduser().resolve())
            for key in ('machine', 'filament', 'process'):
                if getattr(args, key):
                    library.config['presets'][key] = getattr(args, key)
            library.save_config()
            print('Settings saved. Run Doctor and Plan before updating.')
        elif args.action == 'rename':
            matches = [p for p in library.state['projects'].values() if p['id'] == args.project or p['name'] == args.project]
            if len(matches) != 1 or not any(p['index'] == args.plate for p in matches[0]['plates']):
                raise ValueError('Use an exact project name/ID and one of its plate indices.')
            from charm_library.geometry import safe
            name = safe(args.name)
            if not name:
                raise ValueError('Plate name is empty')
            library.config['plate_names'].setdefault(matches[0]['id'], {})[str(args.plate)] = name
            library.save_config()
            print('Name saved; the next update will rebuild and archive the previous managed files.')
        elif args.action == 'use-source':
            matches = [p for p in library.state['projects'].values() if p['id'] == args.project or p['name'] == args.project]
            path = Path(args.path).expanduser().resolve()
            if len(matches) != 1:
                raise ValueError('Use an exact project name or ID from Library → Projects.')
            if not path.is_file() or path.suffix.lower() != '.3mf' or path.name.lower().endswith('.gcode.3mf'):
                raise ValueError('Select an existing editable .3mf project.')
            if any(p['source'] == str(path) and p['id'] != matches[0]['id'] for p in library.state['projects'].values()):
                raise ValueError('That source is already tracked as another project.')
            if matches[0]['source'] != str(path):
                matches[0].setdefault('retired_sources', []).append(matches[0]['source'])
                matches[0]['retired_sources'] = [p for p in matches[0]['retired_sources'] if p != str(path)]
                matches[0]['source'] = str(path)
            from charm_library.engine import atomic_json
            atomic_json(library.state_path, library.state)
            print('Source registered for the existing project. Review Library → Plan, then Update.')
        elif args.action == 'adopt':
            from charm_library.adopt import adopt
            adopt(library, args.folder)
        elif args.action in ('update', 'previews'):
            return library.update(args.project, args.limit, args.force, args.action == 'previews')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print('\nCanceled. Completed projects are retained; interrupted publication resumes on the next update.', file=sys.stderr)
        raise SystemExit(130)
    except (OSError, ValueError, KeyError, IndexError, ImportError, subprocess.CalledProcessError, zipfile.BadZipFile, ET.ParseError) as error:
        print('Error: ' + str(error), file=sys.stderr)
        raise SystemExit(1)

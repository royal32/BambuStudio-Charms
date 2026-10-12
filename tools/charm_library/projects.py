"""Stable, named editable inputs, separate from disposable slicer work files."""
import os
from pathlib import Path
import shutil

from . import geometry, shared


def source_path(library, project):
    if project.get('editable_project'):
        return shared.child(library.shared_root(), project['editable_project'])
    return Path(project['source'])


def promote(library, project, prepared):
    """Register an already validated prepared project without replacing any edits.

    Call under the library lock, then persist the returned entry in the manifest.
    An interrupted registration can reuse an identical copy. Different existing
    contents always get a distinct human-readable name, never an overwrite.
    """
    from .engine import digest
    prepared = Path(prepared)
    sha = digest(prepared)
    root = library.shared_root()
    stem = geometry.safe(project['name']) or 'Project'
    if not stem.endswith(' A1 Mini'):
        stem += ' A1 Mini'
    folder = shared.child(root, 'Projects')
    folder.mkdir(parents=True, exist_ok=True)
    owned = {p.get('editable_project'): p['id'] for p in library.state['projects'].values() if p.get('editable_project')}
    number = 1
    while True:
        name = stem + (f' ({number})' if number > 1 else '') + '.3mf'
        relative = 'Projects/' + name
        target = shared.child(root, relative)
        if relative.casefold() in {p.casefold() for p, owner in owned.items() if owner != project['id']}:
            number += 1
            continue
        try:
            with target.open('xb') as output, prepared.open('rb') as source:
                shutil.copyfileobj(source, output)
                output.flush()
                os.fsync(output.fileno())
        except FileExistsError:
            if digest(target) != sha:
                number += 1
                continue
        if digest(target) != sha or digest(prepared) != sha:
            raise ValueError('Editable project changed during registration: ' + str(target))
        break
    previous = project['source']
    retired = set(project.get('retired_sources', []))
    if previous != str(target):
        retired.add(previous)
    retired.discard(str(target))
    project.update(source=str(target), source_sha256=sha, editable_project=relative,
                   retired_sources=sorted(retired))
    return project

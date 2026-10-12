"""Opt-in integration check against the local slicer and adopted project fixtures.

CHARM_LIBRARY_INTEGRATION=1 .charm-library/venv/bin/python3 -m unittest discover \
    -s tests -p test_charm_library_slicer.py
"""
import copy
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from charm_library.engine import Library, digest
from charm_library.package import validate
from charm_library import projects


@unittest.skipUnless(os.environ.get('CHARM_LIBRARY_INTEGRATION') == '1', 'Opt-in: uses installed Studio and local adopted fixtures')
class SlicerIntegrationTests(unittest.TestCase):
    def test_saved_editable_project_is_used_on_refresh(self):
        existing = Library()
        project = next(p for p in existing.state['projects'].values() if p['name'] == 'Band Mom')
        if not project.get('editable_project'):
            self.skipTest('Historic library has not migrated to named editable projects yet')
        seed = projects.source_path(existing, project)
        with tempfile.TemporaryDirectory(prefix='charm-saved-edit-') as temporary:
            library = Library(Path(temporary) / 'state')
            library.config = copy.deepcopy(existing.config)
            library.config.update(library=str(Path(temporary) / 'sliced'), editable_projects=True,
                                  sources=[str(library.home / 'inbox')], plate_names={}, source_categories={})
            library.initialize()
            source = library.home / 'inbox' / 'Band Mom.3mf'
            shutil.copyfile(seed, source)
            library.save_config()
            with library.locked():
                self.assertEqual(library.update(), 0)
            entry = next(iter(library.state['projects'].values()))
            editable = projects.source_path(library, entry)
            self.assertEqual(editable.name, 'Band Mom A1 Mini.3mf')
            jobs = [Path(p) for p in library.files() if p.endswith('.gcode.3mf')]
            before = {p: digest(p) for p in jobs}
            # Simulate saving a measurable geometry edit in the named project.
            # Scale X in every model mesh by 2%, retaining settings and plates.
            with zipfile.ZipFile(editable) as archive:
                contents = {n: archive.read(n) for n in archive.namelist()}
            ns = {'m': 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'}
            changed = 0
            for name, data in list(contents.items()):
                if not name.endswith('.model'):
                    continue
                model = ET.fromstring(data)
                for vertex in model.findall('.//m:vertex', ns):
                    vertex.set('x', str(float(vertex.get('x')) * 1.02))
                    changed += 1
                contents[name] = ET.tostring(model, encoding='utf-8', xml_declaration=True)
            self.assertGreater(changed, 0)
            with zipfile.ZipFile(editable, 'w', zipfile.ZIP_DEFLATED) as archive:
                for name, data in contents.items():
                    archive.writestr(name, data)
            saved_hash = digest(editable)
            with library.locked():
                rows, presets = library.plan()
                self.assertEqual([(r['action'], r['reason']) for r in rows], [('build', 'Source changed')])
                self.assertEqual(library.update(), 0)
            self.assertEqual(digest(editable), saved_hash, 'Refresh must not overwrite saved project edits')
            self.assertEqual(len(library.state['projects']), 1)
            self.assertTrue(any(digest(p) != sha for p, sha in before.items()))
            for job in jobs:
                validate(job, library.config, presets, single=True)
            with library.locked():
                rows, _ = library.plan()
            self.assertEqual([r['action'] for r in rows], ['skip'])

    def test_real_slice_export_previews_and_incremental_second_run(self):
        existing = Library()
        fixtures = [('Bee', 'Bogg Charm'), ('Grinch', 'Bogg Charm'), ('Wanda (A1 mini Source)', 'Pair Eyewear')]
        with tempfile.TemporaryDirectory(prefix='charm-library-integration-') as temporary:
            library = Library(Path(temporary) / 'state')
            library.config = copy.deepcopy(existing.config)
            library.config.update(library=str(Path(temporary) / 'sliced'),
                                  sources=[str(library.home / 'inbox')], plate_names={}, source_categories={})
            library.initialize()
            for name, category in fixtures:
                project = next(p for p in existing.state['projects'].values() if p['name'] == name)
                source = library.home / 'inbox' / (name + '.3mf')
                shutil.copyfile(project['prepared_input'], source)
                library.config['source_categories'][str(source)] = category
            library.save_config()
            with library.locked():
                result = library.update()
            if result:
                logs = existing.home / 'test-failures' / str(time.time_ns())
                shutil.copytree(temporary, logs)
                print('Integration diagnostics retained: ' + str(logs))
            self.assertEqual(result, 0)
            self.assertEqual(len(library.state['projects']), 3)
            _, _, presets = library.recipes()
            jobs = [Path(p) for p in library.files() if p.endswith('.gcode.3mf')]
            self.assertEqual(len(jobs), 9)
            for job in jobs:
                validate(job, library.config, presets, single=True)
                from charm_library.artwork import read
                self.assertGreater(len(read(job)['regions']), 0)
            before = {p: (digest(p), Path(p).stat().st_mtime_ns) for p in library.files()}
            with library.locked():
                self.assertEqual(library.update(), 0)
            self.assertEqual(before, {p: (digest(p), Path(p).stat().st_mtime_ns) for p in library.files()})


if __name__ == '__main__':
    unittest.main()

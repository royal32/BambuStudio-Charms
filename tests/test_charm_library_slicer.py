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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from charm_library.engine import Library, digest
from charm_library.package import validate


@unittest.skipUnless(os.environ.get('CHARM_LIBRARY_INTEGRATION') == '1', 'Opt-in: uses installed Studio and local adopted fixtures')
class SlicerIntegrationTests(unittest.TestCase):
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
                preview = Path(str(job) + '.preview.json')
                self.assertTrue(preview.exists(), job.name)
                artwork = json.loads(preview.read_text())
                self.assertGreater(len(artwork['regions']), 0)
            before = {p: (digest(p), Path(p).stat().st_mtime_ns) for p in library.files()}
            with library.locked():
                self.assertEqual(library.update(), 0)
            self.assertEqual(before, {p: (digest(p), Path(p).stat().st_mtime_ns) for p in library.files()})


if __name__ == '__main__':
    unittest.main()

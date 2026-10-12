"""Exercise incremental decisions and publication using a bounded fake slicer."""
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from charm_library.engine import Library, atomic_json, digest, signature
from charm_library.prepare import resolve_presets
from charm_library import shared


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name) / 'state'
        self.lib = Library(self.home)
        self.lib.config['library'] = str(Path(self.temp.name) / 'sliced')
        self.lib.initialize()
        self.lib.save_config()
        self.source = self.home / 'inbox/Bee.3mf'
        self.source.write_bytes(b'original geometry')
        self.recipe, self.art = 'slice-v1', 'art-v1'
        self.slices = 0
        self.plate_count = 1

        def recipes(_):
            return self.recipe, self.art, {}

        def prepare(source, folder, family, category, presets, overrides):
            shutil.copyfile(source, folder / 'input.3mf')
            return {'plates': [{'index': i, 'name': (overrides or {}).get(str(i), family + (' SB' if i == 1 else ' Plain'))}
                               for i in range(1, self.plate_count + 1)],
                    'shifts': [{'plate': i, 'dimensions': [40, 40, 5]} for i in range(1, self.plate_count + 1)]}

        def slice_project(config, folder, plan):
            self.slices += 1

        def export(folder, plan, config, presets):
            source = (folder / 'input.3mf').read_bytes()
            (folder / 'prepared.3mf').write_bytes(source)
            result = []
            for plate in plan['plates']:
                path = folder / f"plate-{plate['index']}.gcode.3mf"
                path.write_bytes(b'sliced:' + source + self.recipe.encode() + str(plate['index']).encode())
                result.append((copy.deepcopy(plate), path))
            return result

        def preview(_, prepared, job, plate, policy='auto'):
            return ({'.preview.json': self.art, '.preview.svg': self.art}, '') if policy == 'auto' else ({}, policy)

        for target, function in [('charm_library.engine.Library.recipes', recipes),
                                 ('charm_library.engine.prepare.prepare', prepare),
                                 ('charm_library.engine.prepare.slice_project', slice_project),
                                 ('charm_library.engine.package.export', export),
                                 ('charm_library.engine.Library.preview', preview)]:
            patcher = patch(target, function)
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_update(self, **kwargs):
        with self.lib.locked():
            return self.lib.update(**kwargs)

    def action(self):
        return self.lib.plan()[0][0]['action']

    def entry(self):
        return next(iter(self.lib.state['projects'].values()))

    def test_incremental_noop_preserves_outputs_and_manifest(self):
        self.assertEqual(self.run_update(), 0)
        files = list(self.lib.files()) + [str(self.lib.state_path)]
        before = {p: (digest(p), Path(p).stat().st_mtime_ns) for p in files}
        self.assertEqual(self.action(), 'skip')
        self.assertEqual(self.run_update(), 0)
        self.assertEqual(self.slices, 1)
        self.assertEqual(before, {p: (digest(p), Path(p).stat().st_mtime_ns) for p in files})

    def test_source_change_and_profile_change_rebuild_and_archive(self):
        self.run_update()
        self.source.write_bytes(b'new geometry')
        self.assertEqual(self.action(), 'build')
        self.run_update()
        self.recipe = 'new-profile-or-slicer'
        self.assertEqual(self.action(), 'build')
        self.run_update()
        self.assertEqual(self.slices, 3)
        self.assertTrue(list((self.home / 'archive').rglob('*.gcode.3mf')))

    def test_preview_change_and_missing_sidecar_do_not_slice(self):
        self.run_update()
        self.art = 'new vector algorithm'
        self.assertEqual(self.action(), 'preview')
        self.run_update(previews_only=True)
        next(Path(p) for p in self.lib.files() if p.endswith('.preview.svg')).unlink()
        self.assertEqual(self.action(), 'preview')
        self.run_update()
        self.assertEqual(self.slices, 1)
        self.assertEqual(self.action(), 'skip')

    def test_edited_output_blocks_even_a_forced_rebuild(self):
        self.run_update()
        output = next(Path(p) for p in self.lib.files() if p.endswith('.gcode.3mf'))
        output.write_bytes(b'user edit')
        self.source.write_bytes(b'new source')
        self.assertEqual(self.run_update(force=True), 1)
        self.assertEqual(output.read_bytes(), b'user edit')
        self.assertEqual(self.slices, 1)

    def test_untracked_collision_gets_searchable_distinct_name(self):
        output = Path(self.lib.config['library']) / 'Bee SB.gcode.3mf'
        output.parent.mkdir()
        output.write_bytes(b'old untracked')
        self.run_update()
        self.assertEqual(self.entry()['plates'][0]['filename'], 'Bee Design 2 SB.gcode.3mf')
        self.assertEqual(output.read_bytes(), b'old untracked')

    def test_identical_new_source_is_skipped_against_existing_project(self):
        self.run_update()
        duplicate = self.home / 'inbox/AAA.3mf'
        shutil.copyfile(self.source, duplicate)
        rows, _ = self.lib.plan()
        self.assertEqual({r['action'] for r in rows}, {'skip', 'duplicate'})
        self.run_update()
        self.assertEqual(self.slices, 1)

    def test_missing_source_retains_outputs(self):
        self.run_update()
        outputs = self.lib.files()
        self.source.unlink()
        self.assertEqual(self.run_update(), 1)
        self.assertTrue(all(Path(p).exists() for p in outputs))

    def test_missing_job_rebuilt(self):
        self.run_update()
        next(Path(p) for p in self.lib.files() if p.endswith('.gcode.3mf')).unlink()
        self.assertEqual(self.action(), 'build')
        self.run_update()
        self.assertEqual(self.slices, 2)

    def test_preview_policy_survives_rebuild(self):
        self.run_update()
        self.entry()['plates'][0]['preview_policy'] = 'Reviewed non-flat model'
        atomic_json(self.lib.state_path, self.lib.state)
        self.source.write_bytes(b'new source')
        self.run_update()
        self.assertEqual(self.entry()['plates'][0]['preview_fallback'], 'Reviewed non-flat model')
        self.assertFalse(any(p.endswith('.preview.json') for p in self.lib.files()))

    def test_partial_publication_recovers_without_reslicing(self):
        self.run_update()
        self.source.write_bytes(b'new source')
        self.art = 'new art'
        original = shutil.copyfile
        copies = 0

        def interrupt(source, target, *args, **kwargs):
            nonlocal copies
            if str(target).endswith('.publishing'):
                copies += 1
                if copies == 2:
                    raise OSError('Simulated interruption')
            return original(source, target, *args, **kwargs)

        with patch('charm_library.engine.shutil.copyfile', interrupt):
            self.assertEqual(self.run_update(), 1)
        self.assertTrue(self.lib.journal.exists())
        self.assertEqual(self.run_update(), 0)
        self.assertFalse(self.lib.journal.exists())
        self.assertEqual(self.slices, 2)
        self.assertTrue(all(digest(p) == v['sha256'] for p, v in self.lib.files().items()))

    def test_recovery_refuses_new_external_edits(self):
        self.run_update()
        self.source.write_bytes(b'new source')
        with patch.object(self.lib, 'recover', side_effect=OSError('interrupt')):
            self.assertEqual(self.run_update(), 1)
        output = next(Path(p) for p in self.lib.files() if p.endswith('.gcode.3mf'))
        output.write_bytes(b'edited after interruption')
        with self.assertRaisesRegex(ValueError, 'Publication conflict'):
            self.run_update()
        self.assertEqual(output.read_bytes(), b'edited after interruption')
        self.assertTrue(self.lib.journal.exists())

    def test_lock_rejects_concurrent_operation(self):
        with self.lib.locked():
            with self.assertRaisesRegex(ValueError, 'Another library operation'):
                with Library(self.home).locked():
                    self.fail('Lock allowed concurrent operation')

    def test_bad_project_does_not_abort_other_projects(self):
        (self.home / 'inbox/Good.3mf').write_bytes(b'good')
        from charm_library import prepare
        original = prepare.prepare

        def broken(source, *args):
            if source.resolve() == self.source.resolve():
                raise zipfile.BadZipFile('Invalid project')
            return original(source, *args)

        with patch('charm_library.engine.prepare.prepare', broken):
            self.assertEqual(self.run_update(), 1)
        self.assertEqual(self.entry()['name'], 'Good')
        self.assertEqual(json.loads((self.home / 'last-run.json').read_text())['completed'], 1)

    def test_shared_missing_filename_is_not_reallocated(self):
        self.run_update()
        entry = copy.deepcopy(self.entry())
        entry['id'] = 'another-owner'
        self.lib.state['projects'][entry['id']] = entry
        next(Path(p) for p in self.lib.files() if p.endswith('.gcode.3mf')).unlink()
        target = self.lib.allocate('Bee SB', self.entry()['id'], set())
        self.assertEqual(target.name, 'Bee Design 2 SB.gcode.3mf')

    def test_configured_plate_rename_changes_recipe(self):
        self.run_update()
        self.lib.config['plate_names'][self.entry()['id']] = {'1': 'Honey Bee SB'}
        self.lib.save_config()
        self.assertEqual(self.action(), 'build')
        self.run_update()
        self.assertEqual(self.entry()['plates'][0]['filename'], 'Honey Bee SB.gcode.3mf')

    def test_replace_source_keeps_identity_and_does_not_rediscover_old_version(self):
        self.run_update()
        key = self.entry()['id']
        newer = self.home / 'inbox/Bee v2.3mf'
        newer.write_bytes(b'new release')
        from library_backend import main
        with patch.dict('os.environ', {'CHARM_LIBRARY_HOME': str(self.home)}):
            self.assertEqual(main(['use-source', '--project', 'Bee', '--path', str(newer)]), 0)
        self.lib = Library(self.home)
        rows, _ = self.lib.plan()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['id'], key)
        self.assertEqual(rows[0]['action'], 'build')
        self.run_update()
        self.assertEqual(self.entry()['id'], key)
        self.assertEqual(len(self.lib.plan()[0]), 1)

    def exclude(self, index=1, identifier='deleted-one'):
        entry = self.entry()
        plate = next(p for p in entry['plates'] if p['index'] == index)
        root = self.lib.shared_root()
        record = dict(version=1, kind='exclude', id=identifier, plate=plate['filename'], name=plate['name'],
                      removed_dir='.charm-library/removed/' + identifier,
                      targets=[dict(project_id=entry['id'], plate_index=index, source_sha256=entry['source_sha256'])])
        shared.write_json(root, '.charm-library/events/' + identifier + '.json', record)
        for suffix in ('', '.preview.json', '.preview.svg'):
            source = root / (plate['filename'] + suffix)
            if source.exists():
                target = root / record['removed_dir'] / (plate['filename'] + suffix)
                target.parent.mkdir(parents=True, exist_ok=True)
                source.rename(target)
        return record

    def test_deleted_plate_stays_missing_while_sibling_updates(self):
        self.plate_count = 2
        self.run_update()
        record = self.exclude()
        self.assertEqual(self.action(), 'skip')
        self.assertEqual(self.run_update(), 0)
        self.source.write_bytes(b'new geometry')
        self.assertEqual(self.run_update(force=True), 0)
        self.assertFalse((self.lib.shared_root() / record['plate']).exists())
        sibling = self.lib.shared_root() / self.entry()['plates'][1]['filename']
        self.assertIn(b'new geometry', sibling.read_bytes())
        self.assertEqual(self.action(), 'skip')

    def test_excluded_plate_identity_survives_rename_and_preview_refresh(self):
        self.plate_count = 2
        self.run_update()
        self.exclude()
        self.lib.config['plate_names'][self.entry()['id']] = {'1': 'Renamed Honey Bee SB'}
        self.lib.save_config()
        self.assertEqual(self.run_update(), 0)
        self.assertFalse((self.lib.shared_root() / 'Renamed Honey Bee SB.gcode.3mf').exists())
        self.assertEqual(self.run_update(force=True, previews_only=True), 0)
        self.assertFalse((self.lib.shared_root() / 'Renamed Honey Bee SB.gcode.3mf.preview.json').exists())
        self.assertEqual(self.action(), 'skip')

    def test_reinclude_rebuilds_current_source_after_updates_while_excluded(self):
        self.plate_count = 2
        self.run_update()
        record = self.exclude()
        self.source.write_bytes(b'latest source')
        self.run_update()
        shared.restore(self.lib, record['plate'])
        self.assertEqual(self.action(), 'build')
        self.assertEqual(self.run_update(), 0)
        self.assertIn(b'latest source', (self.lib.shared_root() / record['plate']).read_bytes())
        self.assertEqual(self.action(), 'skip')

    def test_restore_only_cancels_observed_deletions(self):
        self.run_update()
        record = self.exclude()
        shared.restore(self.lib, record['plate'])
        self.exclude(identifier='concurrent-new-delete')
        self.assertEqual([r['id'] for r in shared.exclusions(self.lib.shared_root())], ['concurrent-new-delete'])

    def test_restore_untracked_export_recovers_job_and_previews(self):
        self.run_update()
        record = self.exclude()
        self.lib.state['projects'] = {}
        shared.restore(self.lib, record['plate'])
        for suffix in ('', '.preview.json', '.preview.svg'):
            recovered = self.lib.shared_root() / (record['plate'] + suffix)
            archived = self.lib.shared_root() / record['removed_dir'] / (record['plate'] + suffix)
            self.assertEqual(digest(recovered), digest(archived))
        self.assertFalse(shared.exclusions(self.lib.shared_root()))

    def test_unavailable_recovery_copy_does_not_cancel_exclusion(self):
        self.run_update()
        record = self.exclude()
        self.lib.state['projects'] = {}
        (self.lib.shared_root() / record['removed_dir'] / record['plate']).unlink()
        with self.assertRaisesRegex(ValueError, 'recovery copy is unavailable'):
            shared.restore(self.lib, record['plate'])
        self.assertEqual(len(shared.exclusions(self.lib.shared_root())), 1)

    def test_corrupt_shared_metadata_blocks_updates_without_recreating_plate(self):
        self.run_update()
        record = self.exclude()
        (self.lib.shared_root() / '.charm-library/events/broken.json').write_text('{broken')
        with self.assertRaises(ValueError):
            self.run_update(force=True)
        self.assertFalse((self.lib.shared_root() / record['plate']).exists())

    def test_exclusion_during_interrupted_publication_is_honored_on_recovery(self):
        self.plate_count = 2
        self.run_update()
        self.source.write_bytes(b'new source')
        with patch.object(self.lib, 'recover', side_effect=OSError('interruption')):
            self.assertEqual(self.run_update(), 1)
        record = self.exclude()
        self.assertEqual(self.run_update(), 0)
        self.assertFalse((self.lib.shared_root() / record['plate']).exists())
        self.assertIn(b'new source', (self.lib.shared_root() / self.entry()['plates'][1]['filename']).read_bytes())

    def test_metadata_sync_is_idempotent_and_uses_relative_plate_identity(self):
        self.run_update()
        path = self.lib.shared_root() / '.charm-library/projects' / (self.entry()['id'] + '.json')
        before = path.stat().st_mtime_ns
        self.run_update()
        self.assertEqual(before, path.stat().st_mtime_ns)
        data = json.loads(path.read_text())
        self.assertEqual(data['plates'][0]['index'], 1)
        self.assertEqual(data['plates'][0]['id'], 'Bee SB.gcode.3mf')
        self.assertEqual(data['source'], str(self.source.resolve()))

    def test_source_inside_library_uses_portable_relative_path(self):
        self.run_update()
        root = self.lib.shared_root()
        project = dict(self.entry(), source=str(root / 'Sources/Bee.3mf'))
        shared.sync_projects(root, {project['id']: project})
        path = root / '.charm-library/projects' / (project['id'] + '.json')
        self.assertEqual(json.loads(path.read_text())['source'], 'Sources/Bee.3mf')


class PresetTests(unittest.TestCase):
    def test_inheritance_includes_and_user_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profiles = {
                'system/BBL/machine/base.json': {'printer_model': 'Bambu Lab A1 mini', 'nozzle_diameter': ['0.4']},
                'user/id/machine/printer.json': {'inherits': 'base', 'name': 'printer'},
                'system/BBL/process/default.json': {'layer_height': '0.2'},
                'system/BBL/process/speeds.json': {'speed': '80'},
                'user/id/process/quality.json': {'inherits': 'default', 'include': ['speeds'], 'speed': '60'},
                'user/id/filament/plastic.json': {'filament_flow_ratio': ['0.95']},
            }
            for path, value in profiles.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(json.dumps(value))
            config = {'profile_root': str(root), 'presets': {'machine': 'printer', 'process': 'quality', 'filament': 'plastic'}}
            values = resolve_presets(config)
            self.assertEqual(values['process']['layer_height'], '0.2')
            self.assertEqual(values['process']['speed'], '60')
            profiles['user/id/machine/printer.json']['inherits'] = 'printer'
            (root / 'user/id/machine/printer.json').write_text(json.dumps(profiles['user/id/machine/printer.json']))
            with self.assertRaisesRegex(ValueError, 'inheritance cycle'):
                resolve_presets(config)


if __name__ == '__main__':
    unittest.main()

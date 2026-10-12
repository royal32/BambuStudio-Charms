"""Archive contract shared by the processing tool and Studio's native reader."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import warnings
import zipfile
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from charm_library import artwork


def fixture(path):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>')
        z.writestr('Metadata/plate_5.gcode', '; toolpaths\nG1 X10 Y20\n')
        z.writestr('Metadata/project_settings.config', '{"filament_colour":["#FFFFFF"]}')
        z.writestr('Metadata/slice_info.config', '<config/>')
        z.writestr('Metadata/plate_5.png', b'original printer thumbnail')
        z.comment = b'keep comment'
    return dict(version=2, plate_index=5, width=10, height=10, print_sha256=artwork.fingerprint(path, 5),
                regions=[dict(filament=0, color='#FFFFFF', contours=[[[0, 0], [10, 0], [10, 10]]])])


class EmbeddedArtworkTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.original = self.root / '原作.gcode.3mf'
        self.job = self.root / 'Mamá Bee.gcode.3mf'
        self.data = fixture(self.original)
        artwork.write(self.original, self.job, self.data)

    def test_preserves_all_print_members_and_declares_private_json(self):
        self.assertEqual(artwork.read(self.job), self.data)
        with zipfile.ZipFile(self.original) as a, zipfile.ZipFile(self.job) as b:
            self.assertEqual(b.comment, a.comment)
            self.assertEqual(set(b.namelist()) - {artwork.MEMBER}, set(a.namelist()))
            for name in a.namelist():
                if name != artwork.CONTENT_TYPES:
                    self.assertEqual(a.read(name), b.read(name), name)
            self.assertIn(b'application/json', b.read(artwork.CONTENT_TYPES))

    def test_second_write_identical_and_recompression_keeps_identity(self):
        target = self.root / 'again.3mf'
        artwork.write(self.job, target, self.data)
        self.assertEqual(target.read_bytes(), self.job.read_bytes())
        with zipfile.ZipFile(self.job) as a, zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as b:
            for name in reversed(a.namelist()):
                b.writestr(name, a.read(name))
        self.assertEqual(artwork.read(target), self.data)

    def test_changed_print_contents_reject_artwork(self):
        for changed in ('Metadata/plate_5.gcode', 'Metadata/project_settings.config', 'Metadata/slice_info.config'):
            target = self.root / 'stale.3mf'
            with zipfile.ZipFile(self.job) as a, zipfile.ZipFile(target, 'w') as b:
                for name in a.namelist():
                    b.writestr(name, a.read(name) + (b'changed' if name == changed else b''))
            with self.assertRaisesRegex(ValueError, 'does not match'):
                artwork.read(target)

    def test_missing_or_removed_preview_is_png_fallback(self):
        self.assertIsNone(artwork.read(self.original))
        target = self.root / 'fallback.3mf'
        artwork.write(self.job, target, None)
        self.assertIsNone(artwork.read(target))
        self.assertEqual(artwork.fingerprint(target, 5), self.data['print_sha256'])
        with zipfile.ZipFile(target) as z:
            self.assertNotIn(b'application/json', z.read(artwork.CONTENT_TYPES))

    def test_duplicate_members_and_stale_write_rejected(self):
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            with zipfile.ZipFile(self.job, 'a') as z:
                z.writestr('Metadata/plate_5.gcode', 'duplicate')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            artwork.read(self.job)
        bad = dict(self.data, print_sha256='a' * 64)
        with self.assertRaisesRegex(ValueError, 'does not match'):
            artwork.write(self.original, self.root / 'bad.3mf', bad)

    def test_invalid_geometry_and_format_rejected(self):
        for changes in (dict(version=1), dict(width=0), dict(print_sha256='bad'), dict(regions=[]), dict(plate_index=True)):
            with self.assertRaises(ValueError):
                artwork.validate(dict(self.data, **changes))
        data = copy.deepcopy(self.data)
        data['regions'][0]['contours'][0][0] = [100, 0]
        with self.assertRaises(ValueError):
            artwork.validate(data)

    def test_content_type_declaration_does_not_change_model_xml_namespaces(self):
        model_ns = 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'
        ET.register_namespace('', model_ns)
        model = ET.Element('{' + model_ns + '}model')
        before = ET.tostring(model)
        artwork.content_types(b'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>', True)
        self.assertEqual(ET.tostring(model), before)


if __name__ == '__main__':
    unittest.main()

"""Geometry checks for the mesh-section exporter; requires its dependencies."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
import unittest
import xml.etree.ElementTree as ET
import zipfile

import numpy as np
from shapely.geometry import Point, Polygon

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
spec = importlib.util.spec_from_file_location("vector_preview", Path(__file__).resolve().parents[1] / "tools/generate_plate_vector_preview.py")
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


def cube(lo, hi):
    vertices = np.asarray([[x, y, z] for z in [lo[2], hi[2]] for y in [lo[1], hi[1]] for x in [lo[0], hi[0]]], dtype=float)
    faces = np.asarray([[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5], [0, 4, 5], [0, 5, 1],
                        [2, 3, 7], [2, 7, 6], [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]])
    return vertices, faces


def negative_volume_fixture(folder, negative_first, cutter_width=2):
    """A transformed cutter crosses two colors; an adjacent object is independent."""
    uri = preview.NS['m']
    def tag(name):
        return f'{{{uri}}}{name}'
    model = ET.Element(tag('model'))
    resources = ET.SubElement(model, tag('resources'))
    for oid, bounds in [(1, ([0, 0, 0], [10, 10, 2])), (2, ([0, 0, 0], [5, 10, 2])),
                        (3, ([0, 0, -1], [cutter_width, 2, 3]))]:
        obj = ET.SubElement(resources, tag('object'), id=str(oid))
        mesh = ET.SubElement(obj, tag('mesh'))
        vertices = ET.SubElement(mesh, tag('vertices'))
        triangles = ET.SubElement(mesh, tag('triangles'))
        v, f = cube(*bounds)
        for xyz in v:
            ET.SubElement(vertices, tag('vertex'), **dict(zip(['x', 'y', 'z'], map(str, xyz))))
        for ids in f:
            ET.SubElement(triangles, tag('triangle'), **dict(zip(['v1', 'v2', 'v3'], map(str, ids))))
    for oid, parts in [(10, [3, 1, 2] if negative_first else [1, 2, 3]), (20, [1])]:
        obj = ET.SubElement(resources, tag('object'), id=str(oid))
        components = ET.SubElement(obj, tag('components'))
        for part in parts:
            attrs = {'objectid': str(part)}
            if part == 3:
                attrs['transform'] = '1 0 0 0 1 0 0 0 1 4 4 0'
            ET.SubElement(components, tag('component'), **attrs)
    build = ET.SubElement(model, tag('build'))
    ET.SubElement(build, tag('item'), objectid='10')
    ET.SubElement(build, tag('item'), objectid='20', transform='1 0 0 0 1 0 0 0 1 10 0 0')
    settings = ET.fromstring('''<config><object id="10">
        <part id="1" subtype="normal_part"><metadata key="extruder" value="1"/></part>
        <part id="2" subtype="normal_part"><metadata key="extruder" value="2"/></part>
        <part id="3" subtype="negative_part"><metadata key="extruder" value="0"/></part>
        </object><plate><metadata key="plater_id" value="1"/></plate></config>''')
    for oid in [10, 20]:
        instance = ET.SubElement(settings.find('plate'), 'model_instance')
        ET.SubElement(instance, 'metadata', key='object_id', value=str(oid))
        ET.SubElement(instance, 'metadata', key='instance_id', value='0')
    project, job = folder / 'project.3mf', folder / 'job.gcode.3mf'
    with zipfile.ZipFile(project, 'w') as archive:
        archive.writestr('3D/3dmodel.model', ET.tostring(model))
        archive.writestr('Metadata/model_settings.config', ET.tostring(settings))
        archive.writestr('Metadata/project_settings.config', json.dumps({'filament_colour': ['#FF0000', '#FFFFFF']}))
    with zipfile.ZipFile(job, 'w') as archive:
        archive.writestr('Metadata/plate_1.gcode', 'G1 X0 Y0')
        archive.writestr('Metadata/project_settings.config', '{}')
        archive.writestr('Metadata/slice_info.config', '<config><filament id="1" color="#FF0000" used_g="1"/>'
                         '<filament id="2" color="#FFFFFF" used_g="1"/></config>')
    return project, job


def region_geometry(region):
    shape = Polygon()
    for ring in region['contours']:
        shape = shape.symmetric_difference(Polygon(ring))
    return shape


class MeshSectionTests(unittest.TestCase):
    def test_exact_geometry_and_empty_slice(self):
        v, f = cube([0, 0, 0], [10, 20, 2])
        self.assertAlmostEqual(preview.section(v, f, .1).area, 200)
        self.assertTrue(preview.section(v, f, 3).is_empty)

    def test_nested_shell_keeps_hole(self):
        a, fa = cube([0, 0, 0], [10, 10, 2])
        b, fb = cube([3, 3, 0], [7, 7, 2])
        shape = preview.section(np.concatenate([a, b]), np.concatenate([fa, fb + len(a)]), .1)
        self.assertAlmostEqual(shape.area, 84)
        self.assertFalse(shape.contains(Point(5, 5)))
        self.assertTrue(shape.contains(Point(1, 1)))

    def test_3mf_transform_order(self):
        m = preview.matrix("0 1 0 -1 0 0 0 0 1 12 30 2")
        point = m @ np.asarray([3, 4, 1, 1])
        np.testing.assert_allclose(point, [8, 33, 3, 1])

    def test_slicer_negative_volume_cuts_all_colors_on_both_faces(self):
        with tempfile.TemporaryDirectory() as folder:
            for negative_first in [False, True]:
                project, job = negative_volume_fixture(Path(folder), negative_first)
                for face in ['bottom', 'top']:
                    with self.subTest(negative_first=negative_first, face=face):
                        data = preview.generate(project, job, 1, face)
                        shapes = {r['filament']: region_geometry(r) for r in data['regions']}
                        self.assertAlmostEqual(shapes[0].area, 148, places=3)
                        self.assertAlmostEqual(shapes[1].area, 48, places=3)
                        for x in [4.5, 5.5]:
                            hole = Point(20 - x if face == 'bottom' else x, 5)
                            self.assertTrue(all(not s.covers(hole) for s in shapes.values()))
                        self.assertIn('fill-rule="evenodd"', preview.svg(data))

    def test_negative_volume_does_not_cut_an_adjacent_object(self):
        with tempfile.TemporaryDirectory() as folder:
            project, job = negative_volume_fixture(Path(folder), False, cutter_width=8)
            data = preview.generate(project, job, 1)
            shapes = {r['filament']: region_geometry(r) for r in data['regions']}
            self.assertAlmostEqual(shapes[0].area, 140, places=3)
            self.assertFalse(shapes[0].covers(Point(20 - 8, 5)))
            self.assertTrue(shapes[0].covers(Point(20 - 11, 5)))

    def test_svg_retains_independent_equal_color_slots(self):
        data = {"width": 10, "height": 10, "regions": [
            {"filament": n, "color": "#FF0000", "contours": [[[n, 0], [n + 1, 0], [n, 1]]]} for n in [0, 3]]}
        svg = preview.svg(data)
        self.assertIn('data-filament="0"', svg)
        self.assertIn('data-filament="3"', svg)
        self.assertEqual(svg.count('fill="#FF0000"'), 2)


if __name__ == "__main__":
    unittest.main()

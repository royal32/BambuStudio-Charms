"""Geometry checks for the mesh-section exporter; requires its dependencies."""
import importlib.util
from pathlib import Path
import unittest

import numpy as np
from shapely.geometry import Point

spec = importlib.util.spec_from_file_location("vector_preview", Path(__file__).resolve().parents[1] / "tools/generate_plate_vector_preview.py")
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


def cube(lo, hi):
    vertices = np.asarray([[x, y, z] for z in [lo[2], hi[2]] for y in [lo[1], hi[1]] for x in [lo[0], hi[0]]], dtype=float)
    faces = np.asarray([[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5], [0, 4, 5], [0, 5, 1],
                        [2, 3, 7], [2, 7, 6], [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]])
    return vertices, faces


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

    def test_svg_retains_independent_equal_color_slots(self):
        data = {"width": 10, "height": 10, "regions": [
            {"filament": n, "color": "#FF0000", "contours": [[[n, 0], [n + 1, 0], [n, 1]]]} for n in [0, 3]]}
        svg = preview.svg(data)
        self.assertIn('data-filament="0"', svg)
        self.assertIn('data-filament="3"', svg)
        self.assertEqual(svg.count('fill="#FF0000"'), 2)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Create app-only vector artwork for a flat plate from its prepared 3MF.

Requires numpy and shapely (2.x). This takes a section 0.1 mm inside each
object's front face, not a trace of a low-resolution PNG. Later colored parts
carve earlier parts, and negative volumes cut holes. Painted meshes are
explicitly rejected rather than silently losing their filament assignments.

The .gcode.3mf is read only. Keep its .preview.json sidecar beside it. The SVG
is useful for inspection; Studio renders the validated JSON geometry directly.
"""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile

import numpy as np
from shapely import affinity, make_valid
from shapely.geometry import LineString, Polygon, MultiPolygon, GeometryCollection
from shapely.ops import polygonize, unary_union

NS = {"m": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"}
P = "{http://schemas.microsoft.com/3dmanufacturing/production/2015/06}"


def metadata(node):
    return {m.get("key"): m.get("value") for m in node.findall("metadata")}


def matrix(text):
    if not text:
        return np.eye(4)
    a = np.asarray([float(x) for x in text.split()]).reshape(4, 3)
    result = np.eye(4)
    result[:3, :] = a.T
    return result


def polygons(geometry):
    if isinstance(geometry, Polygon):
        yield geometry
    elif isinstance(geometry, (MultiPolygon, GeometryCollection)):
        for child in geometry.geoms:
            yield from polygons(child)


def section(vertices, triangles, z):
    """Intersect triangles, polygonize closed contours, retain even-odd faces."""
    v = vertices[triangles]
    v = v[(v[:, :, 2].min(axis=1) < z) & (v[:, :, 2].max(axis=1) > z)]
    if not len(v):
        return Polygon()
    points = [[] for _ in range(len(v))]
    for a, b in [(0, 1), (1, 2), (2, 0)]:
        active = (v[:, a, 2] > z) != (v[:, b, 2] > z)
        ids = np.flatnonzero(active)
        t = (z - v[active, a, 2]) / (v[active, b, 2] - v[active, a, 2])
        xy = v[active, a, :2] + t[:, None] * (v[active, b, :2] - v[active, a, :2])
        for i, p in zip(ids, xy):
            points[i].append(tuple(np.round(p, 5)))
    segments = list({tuple(sorted(p)) for p in points if len(p) == 2 and p[0] != p[1]})
    if not segments:
        return Polygon()
    candidates = list(polygonize(unary_union([LineString(s) for s in segments])))
    starts = np.asarray([s[0] for s in segments])
    ends = np.asarray([s[1] for s in segments])
    selected = []
    for face in candidates:
        x, y = face.representative_point().coords[0]
        crossing = (starts[:, 1] > y) != (ends[:, 1] > y)
        a, b = starts[crossing], ends[crossing]
        right = x < (b[:, 0] - a[:, 0]) * (y - a[:, 1]) / (b[:, 1] - a[:, 1]) + a[:, 0]
        if np.count_nonzero(right) % 2:
            selected.append(face)
    if not selected:
        raise ValueError("Mesh section did not produce closed regions")
    return make_valid(unary_union(selected))


def generate(project, job, plate_index, face="bottom"):
    with zipfile.ZipFile(project) as archive:
        settings = ET.fromstring(archive.read("Metadata/model_settings.config"))
        config = json.loads(archive.read("Metadata/project_settings.config"))
        docs = {n: ET.fromstring(archive.read(n)) for n in archive.namelist() if n.endswith(".model")}
    with zipfile.ZipFile(job) as archive:
        sliced = ET.fromstring(archive.read("Metadata/slice_info.config"))
        sliced_colors = {int(f.get("id")) - 1: f.get("color", "")[:7].upper()
                         for f in sliced.iter("filament") if float(f.get("used_g", 0)) > 0}
    objects = {(n, o.get("id")): o for n, doc in docs.items() for o in doc.findall("m:resources/m:object", NS)}
    part_settings = {p.get("id"): (p.get("subtype"), metadata(p)) for o in settings.findall("object") for p in o.findall("part")}
    root_settings = {o.get("id"): metadata(o) for o in settings.findall("object")}
    plate = next(p for p in settings.findall("plate") if int(metadata(p)["plater_id"]) == plate_index)
    builds = {}
    for item in docs["3D/3dmodel.model"].findall("m:build/m:item", NS):
        builds.setdefault(item.get("objectid"), []).append(item)
    meshes = {}

    def visit(name, oid, transform, inherited):
        node = objects[(name, oid)]
        mesh = node.find("m:mesh", NS)
        subtype, cfg = part_settings.get(oid, ("normal_part", {}))
        filament = int(cfg.get("extruder", 0)) or inherited
        if mesh is not None:
            if (name, oid) not in meshes:
                ts = mesh.findall("m:triangles/m:triangle", NS)
                if any(any(("paint_color" in k.lower() or "mmu_segmentation" in k.lower()) and value for k, value in t.attrib.items()) for t in ts):
                    raise ValueError("Painted mesh needs a paint-aware vector exporter; use the existing thumbnail for this project")
                vertices = np.asarray([[float(v.get(k)) for k in ["x", "y", "z"]] for v in mesh.findall("m:vertices/m:vertex", NS)])
                triangles = np.asarray([[int(t.get(k)) for k in ["v1", "v2", "v3"]] for t in ts])
                meshes[(name, oid)] = vertices, triangles
            vertices, triangles = meshes[(name, oid)]
            vertices = vertices @ transform[:3, :3].T + transform[:3, 3]
            yield subtype, filament - 1, vertices, triangles
        for c in node.findall("m:components/m:component", NS):
            yield from visit(c.get(P + "path", name).lstrip("/"), c.get("objectid"), transform @ matrix(c.get("transform")), filament)

    regions = {}
    for model_instance in plate.findall("model_instance"):
        data = metadata(model_instance)
        item = builds[data["object_id"]][int(data["instance_id"])]
        if item.get("printable", "1") == "0":
            continue
        inherited = int(root_settings.get(data["object_id"], {}).get("extruder", 1)) or 1
        parts = list(visit(item.get(P + "path", "3D/3dmodel.model").lstrip("/"), item.get("objectid"), matrix(item.get("transform")), inherited))
        positive = [v for kind, _, v, _ in parts if kind == "normal_part"]
        lo = min(v[:, 2].min() for v in positive)
        hi = max(v[:, 2].max() for v in positive)
        depth = min(.1, (hi - lo) / 3)
        z = lo + depth if face == "bottom" else hi - depth
        current, cuts = {}, []
        for kind, filament, vertices, triangles in parts:
            if kind not in ["normal_part", "negative_part"]:
                continue
            shape = section(vertices, triangles, z)
            if shape.is_empty:
                continue
            if kind == "negative_part":
                cuts.append(shape)
                continue
            if not 0 <= filament < len(config["filament_colour"]):
                raise ValueError("Invalid source filament")
            # Prepared project order is also the slicer's carving priority.
            for old in current:
                current[old] = current[old].difference(shape)
            current[filament] = current.get(filament, Polygon()).union(shape)
        cut = unary_union(cuts)
        for filament, shape in current.items():
            shape = make_valid(shape.difference(cut))
            regions[filament] = regions.get(filament, Polygon()).union(shape)
    regions = {f: s for f, s in regions.items() if not s.is_empty and s.area > 1e-5}
    if not regions:
        raise ValueError("No front-face geometry")
    bounds = unary_union(list(regions.values())).bounds
    xmin, ymin, xmax, ymax = bounds
    result = {"version": 1, "plate_index": plate_index, "width": round(xmax - xmin, 5), "height": round(ymax - ymin, 5),
              "source_md5": hashlib.md5(job.read_bytes()).hexdigest(), "source_project": project.name,
              "view": face, "section_depth_mm": depth, "regions": []}
    for filament, shape in sorted(regions.items()):
        if sliced_colors.get(filament) != config["filament_colour"][filament][:7].upper():
            raise ValueError("Source filament does not match the sliced job; select the corresponding prepared project and plate")
        # Looking through the bed at a face-down model mirrors X. Screen Y is down.
        shape = affinity.affine_transform(shape, [-1 if face == "bottom" else 1, 0, 0, -1, xmax if face == "bottom" else -xmin, ymax])
        contours = []
        for polygon in polygons(shape.simplify(.002, preserve_topology=True)):
            for ring in [polygon.exterior, *polygon.interiors]:
                contours.append([[round(x, 5), round(y, 5)] for x, y in list(ring.coords)[:-1]])
        if contours:
            result["regions"].append({"filament": filament, "color": config["filament_colour"][filament][:7], "contours": contours})
    return result


def svg(preview):
    w, h = preview["width"], preview["height"]
    paths = []
    for region in preview["regions"]:
        d = "".join("M" + "L".join(f"{x},{y}" for x, y in ring) + "Z" for ring in region["contours"])
        paths.append(f'<path data-filament="{region["filament"]}" fill="{region["color"]}" fill-rule="evenodd" d="{d}"/>')
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-w*.04} {-h*.04} {w*1.08} {h*1.08}">' + "".join(paths) + "</svg>"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("job", type=Path)
    parser.add_argument("--plate", type=int, required=True)
    parser.add_argument("--face", choices=["bottom", "top"], default="bottom")
    parser.add_argument("--output", type=Path, help="Output basename; defaults to the sliced job path")
    args = parser.parse_args()
    preview = generate(args.project, args.job, args.plate, args.face)
    output = str(args.output or args.job)
    Path(output + ".preview.json").write_text(json.dumps(preview, separators=(",", ":")) + "\n")
    Path(output + ".preview.svg").write_text(svg(preview) + "\n")
    print(json.dumps({"job": args.job.name, "regions": len(preview["regions"]), "points": sum(len(r) for s in preview["regions"] for r in s["contours"]), "output": output}))

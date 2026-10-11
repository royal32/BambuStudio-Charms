# Plate vector previews

The sliced library and Send print job dialog use an optional app-only sidecar:
`Example.gcode.3mf.preview.json`, beside `Example.gcode.3mf`. The sliced archive,
its G-code, and its standard printer thumbnails are never modified. Copy the JSON
along with its job when moving a library. The accompanying SVG is for inspection.

Generate a preview from the **same prepared project and plate** that produced the
sliced job:

```sh
python3 -m pip install -r tools/vector-preview-requirements.txt
python3 tools/generate_plate_vector_preview.py 'Prepared Project.3mf' \
  'Example.gcode.3mf' --plate 5 --face bottom
```

The generator reads the actual transformed mesh geometry. It sections each
object 0.1 mm inside its bottom face (or top face with `--face top`), applies
prepared-part carving priority and negative cuts, and generates disjoint colored
regions with holes. This is intended for flat, face-down charms and eyewear.
It is not a general 3D hidden-surface renderer. Painted color segmentation is
rejected; such jobs continue to use their standard thumbnail. Check the SVG
against the project before publishing new sidecars.

Version 1 JSON contains `plate_index` (one based), `width`, `height` in millimeters,
`source_md5` of the whole sliced archive, and `regions`. Each region has a zero-based
`filament`, a hex `color`, and `contours` of `[x,y]` points in screen coordinates.
Contours use even-odd filling, preserving lens openings and other holes.

Studio validates the source digest and plate index before using the artwork.
Missing, malformed, oversized or stale sidecars fall back to the standard PNG.
Library Refresh also detects sidecar additions, edits and removals. The native
dialog draws vector paths at display resolution, uses those same contours for
hit testing, and routes clicks through the existing AMS mapping controls. An
exact region hit takes priority over the two-DIP tolerance near an edge. Regions
sharing a sliced filament highlight and change together; equal visible RGB does
not combine different filament IDs. Existing material swatches remain available.

Hovered filament regions use animated black/white marching ants, including their
hole boundaries. Animation runs only while a visible preview is hovered.
Preferences → 3D Settings → **Use AMS slot colors in print previews** controls
live recoloring in the print dialog (both vector and PNG previews). It defaults
to disabled, preserving the original model colors while slot mapping still works.

Run the geometry/picking tests (add sidecar paths to test generated plates):

```sh
c++ -std=c++17 -Isrc tests/plate_vector_preview.test.cpp -o /tmp/vector-preview-test
/tmp/vector-preview-test '/path/Example.gcode.3mf.preview.json'
python3 -m unittest discover -s tests -p 'test_vector_preview_generator.py'
```

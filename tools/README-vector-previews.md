# Plate vector previews

The sliced library and Send print job dialog read app artwork embedded at
`Metadata/charm_library/preview.json` inside each `.gcode.3mf`. Copying a sliced
file carries its clickable vector preview with it. Studio generates SVG on demand;
no companion JSON or SVG is needed. Printer PNGs, G-code and print settings remain
unchanged. Source-project links, exclusions and feedback remain shared library
records in `.charm-library`, alongside editable projects in `Projects`.

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
rejected; such jobs continue to use their standard thumbnail. Review the preview
against the project before publishing new plates. The command stages a new archive
and atomically replaces the job only after verifying its embedded artwork.

Version 2 JSON contains `plate_index` (one based), `width`, `height` in millimeters,
`print_sha256`, and `regions`. Each region has a zero-based
`filament`, a hex `color`, and `contours` of `[x,y]` points in screen coordinates.
Contours use even-odd filling, preserving lens openings and other holes.

The fingerprint hashes the sorted required members `Metadata/plate_N.gcode`,
`Metadata/project_settings.config` and `Metadata/slice_info.config`. Each member
contributes its UTF-8 name, a NUL byte, its unsigned 64-bit big-endian uncompressed
length, then its bytes. ZIP order, timestamps, compression and preview changes do
not affect this identity. Studio validates the fingerprint and plate index before
using artwork. Missing, malformed, oversized or stale artwork falls back to the
standard PNG. Library Refresh detects changes to the sliced archive. The native
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

Run the geometry, archive and picking tests:

```sh
c++ -std=c++17 -Isrc tests/plate_vector_preview.test.cpp -o /tmp/vector-preview-test
/tmp/vector-preview-test
python3 -m unittest discover -s tests -p 'test_vector_preview_generator.py'
python3 -m unittest discover -s tests -p 'test_embedded_artwork.py'
```

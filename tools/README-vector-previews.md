# Plate vector previews

The sliced library and Send print job dialog read app artwork embedded at
`Metadata/charm_library/preview.json` inside each `.gcode.3mf`. Copying a sliced
file carries its clickable vector preview with it. Studio generates SVG on demand;
no companion JSON or SVG is needed. Printer PNGs, G-code and print settings remain
unchanged. Source-project links, exclusions and feedback remain shared library
records in `.charm-library`, alongside editable projects in `Projects`.

Studio now generates the artwork automatically when exporting a freshly sliced
editable plate, including named per-plate exports and print-job preparation.
It uses the live model, exact exported instance set and current filament settings,
so saving the project first or running a terminal command is unnecessary. Each
normal re-export regenerates the artwork. This is native C++ and requires no
Python installation on the printing computer. Existing sliced-only files retain
their existing artwork; they cannot regenerate source geometry they do not have.

Native generation is limited to flat, face-down geometry: each object's height
must be at most 25 mm and one quarter of its shorter XY span. Painted parts,
parameter modifiers, mixed filaments, incompatible region-material overrides,
oversized artwork or failed packaging retain the normal PNG. Native geometry
uses the slicer's mesh section/boolean routines, applies instance/part transforms,
keeps part carving priority and subtracts negative volumes independently for each
object. Different slot IDs remain distinct even when their RGB colors match.

The completed archive is staged separately, print-member bytes are copied
unchanged, and digest/plate/filament validation succeeds before atomic publication.
A preview failure keeps the original successful print export with its PNG.

For offline library processing, the Python tool remains available. Generate a
preview from the **same prepared project and plate** that produced the sliced job:

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

A single-plate export uses `Metadata/charm_library/preview.json`, preserving the
original one-based plate index even when it is not plate 1. A multi-plate export
uses `Metadata/charm_library/plate_N.preview.json` for each supported plate, with
the same v2 geometry/digest schema. The send dialog loads the selected plate's
member. The library keeps the normal composite PNG for multi-plate cards instead
of displaying one plate as if it represented the whole archive. Re-exporting to
the same filename invalidates the dialog's vector cache using file time and size.

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
python3 tools/run_studio_assistant_model_checks.py \
  --test-source tests/plate_vector_export.test.cpp --check-name vector-preview-native-checks \
  'Projects/Namaste.3mf' 'Namaste.gcode.3mf'
```

The native export fixture checks transformed instance selection, independent
equal-color slots, negative holes, unsupported geometry, single/multiple plate
packaging, re-export regeneration, portable copies, stale/mismatched rejection
and unchanged print members. Its optional project/job arguments are read-only;
all outputs go under the ignored build directory. Independent Python archive
validation can read the native fixture's embedded single-plate preview too.

# Assistant modeling controls — 2026-10-29

Added `plane_cut`, `add_primitive`, `delete_parts`, and `model_section` to the
assistant's validated tool surface. Geometry edits require the current revision,
use stable object/instance references, and take native Undo snapshots. Rebuilt
parts get new IDs for renderer/slicer invalidation; returned inspection supplies
their new IDs. Negative volumes remain editable in saved projects.

Coordinates and sizes are physical build-plate millimeters for the specified
reference instance. Cylinder X/Y are diameter and Z is length. The tools account
for part and instance transforms, including rotation, mirror and nonuniform scale.
Edits affect every instance sharing that object's parts. Native last-part removal
may fold the volume transform into the instance while retaining physical placement.
No implicit bed drop, profile changes, or manufacturing tolerances are applied.

The assistant's instructions now distinguish separate-part deletion, connected
geometry requiring a plane cut, printable primitives, and subtractive volumes.
They require visual/geometric grounding and outcome checks instead of claiming
that these controls are unavailable. Inspection includes per-instance part bounds;
model views support all six cardinal directions plus isometric. Cross-sections
report contours and physical areas with negative volumes subtracted.

## Verification

- Native synthetic fixture: a single connected 20 × 30 × 3 mm body with a hollow
  projecting loop. Cut back the loop and create a centered 2 mm through-hole.
  Cross-sections at three depths and the actual slicer's layer polygons verify
  the opening. The saved live assistant project independently passes the same
  diameter, centering and 3 mm edge-offset checks after reloading.
- Native checks cover angled cuts, physical cylinder dimensions under rotated,
  mirrored/scaled instances and parts, both retained half-spaces, whole-part
  removal, printable boxes/spheres, negative-part removal, invalid/stale targets,
  dimension checks, no-op/all-material cut rejection, and unrelated mesh/config
  preservation. Retained surface paint is reprojected using the native mechanism;
  unrelated paint remains unchanged.
- The synthetic geometry/slicing suite also passed on an Intel Mac
  running macOS 12.6, using only disposable test files. No print was sent.
- Client/session tests pass, including new tool exposure, mutation classification,
  missing-revision rejection and malformed coordinate-array rejection.
- ARM and Intel builds pass. The signed Intel app passes the macOS 12.6 bundle
  architecture, dependency and deployment audit.

## Live Luna Max trials

The original user model was unavailable, so the native fixture was imported as
`Loop earring before.stl`. Prompt requested loop removal, a 2 mm diameter hole
centered 3 mm below the new edge, and preservation of the body/color/position.

The first live trial chose the correct cut and negative cylinder, measured the
opening at three depths, and sliced successfully. It exposed a stale Prepare
renderer: the cut mesh had kept its cached geometry ID. Fixed by assigning new
volume IDs after mesh replacement, matching native remeshing.

Repeating the same prompt on the corrected signed build passed. Inspection shows
the unchanged body dimensions and placement, correct negative-cylinder dimensions,
and measured hole contours. The fresh rendered top image shows the rectangular
body without the old loop and with the new opening. Luna reported the successful
slice and existing instant-timelapse quality warning accurately. No extra modeling
confirmation was needed for the authorized cut and hole.

The final live follow-up also passed: Luna added a printable cylinder with 3 mm
diameter and 2 mm height at the body's center/on its top surface, rendered it,
then removed only its new part ID after the native deletion confirmation. The
body and negative cylinder remained, and the final slice and section passed.
Native Undo restored the peg and Redo removed it again. Saved archive inspection
confirmed two/three/two volumes for final/Undo/Redo, with the negative hole intact.
The final and Redo projects also passed independent native reload/section checks.

## Delivery

Installed the signed ARM app in the local `/Applications/BambuStudio.app` and
the signed Intel app at the same path on the Intel Mac. Previous apps were retained
in `build/arm64/Installed-Backup-20261029/BambuStudio.app` locally and
`~/Downloads/Charms-Assistant-Geometry-20261029/Previous BambuStudio.app` on the
Intel Mac. Verified the transferred archive digest and installed Intel executable:
`def018d59c4050ad0b5952f990a1c0714d568d2905beca11a51df979bc624c6b`.
The Intel Mac app's signature verifies; remote GUI/authenticated Connect launch was
not attempted over SSH. Native Intel checks passed on that machine before
installation. No shared library projects or print jobs were changed.

## Limits

These are geometric primitives and plane trims, not arbitrary sculpting or face
painting. Paint on rebuilt/capped faces uses approximate reprojection and merits
inspection. A negative primitive intersecting an object's bounds is not proof of
a through-hole or adequate remaining material; the assistant must check sections
and slicing. A successful slice does not certify structural strength.

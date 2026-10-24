# Native library actions — 2026-10-24

Studio exports the current or all sliced printable plates into the selected
library, saves a portable editable project, and registers stable project/plate
identities. The library context menu can reopen that saved source, slice it and
publish its registered files again. Native print menus retain the existing print
and export selections and expose the two immediate library actions. Exclusions,
aliases and filename ownership remain part of the shared folder protocol.

Automated checks passed:

- Native storage/source-link checks: repeat and partial additions, collisions,
  aliases, exclusions, Unicode names, successful replacement and rollback after
  a failed publication. The storage checks also ran on the actual macOS 12.6 Intel Mac
  with synthetic temporary files.
- Python library suite: 35 tests. Native registrations survive CLI refresh;
  changed native sources are directed to Studio instead of normalization.
- Library web UI: 6 tests. Embedded archive artwork: 7 tests.
- Native geometry/archive fixture: transformed instances, equal-RGB independent
  slots, negative holes, unsupported geometry fallback, portable single/multiple
  plate archives, re-export/stale rejection, unchanged print-member bytes, and
  the actual Namaste geometry at 93.04349 × 70.74078 mm.
- ARM and Intel application builds. Signed Intel package passes the macOS 12.6
  dependency/deployment audit. A fresh Windows build was not run.

Live checks used copies under `build/library-actions-live`, never the real
collection. The native print menu was visible and keyboard accessible; both
library actions were disabled before slicing and enabled after slicing.
**Add plate to library** published Namaste, automatically showed its card and
linked `Projects/Namaste.3mf`. Independent archive inspection verified the
A1 Mini, Atomic PLA and Charms profiles and digest-bound white/black/red vectors.

An earlier regeneration review exposed the progress dialog filtering out the
slicer's custom completion events. Cancel stopped that test and retained the
existing job and registration. The final loop explicitly pumps application
events while its modal dialog and busy guard prevent project edits. The Mac
locked before the corrected end-to-end regeneration and two-plate Bee export
could be checked. Those two live checks remain pending; automated checks and
successful builds do not substitute for them.

The empty development session was stopped and the original iCloud library-folder
preference restored. Installed applications and real source/print files were not
replaced. Review bundles are in
`build/<arch>/Library-Actions-Review/BambuStudio.app`.

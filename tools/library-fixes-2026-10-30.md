# Library lookup, filament shortcuts and assistant layout

Opening a source project no longer scans every project registration in iCloud.
The shared library now has a portable per-plate lookup, maintained by both
publishers and checked against the authoritative project record. Read-only
source/reveal lookup has a 15-second deadline; a late result cannot unexpectedly
open a project after the error has been shown.

The existing collection's lookup covers 1,723 registered sliced plates. The
previously unregistered `Namaste.gcode.3mf` retains the Locate-source flow.
Project files and sliced archives were not rewritten by the lookup migration.

Number keys accept focus in the native object tree's child controls. Name
editing retains normal text input; the read-only filament picker accepts slot
digits. The canvas routes eligible digits before ImGui consumes key-down.
Inactive Cocoa editors are canceled before assigning a filament. Closing an
unchanged filament picker no longer reapplies the parent filament and erases
part overrides. Starting a plate-name edit now updates that plate's row instead
of whichever model is selected.

Assistant model/menu controls share a height, quick actions have equal widths
and a real gap, and Send aligns with the composer. Native macOS round-rect
bezels avoid the fixed outsets that previously misaligned the controls.

## Validation

- Native shared-storage regression checks passed on ARM and on the Intel
  macOS 12.6 machine; Python's 35 library tests and all 6 library JS tests passed.
- Real shared-folder source lookup for `# Boy Mom Circle.gcode.3mf` found
  `Projects/Boy Mom Girl Mom A1 Mini.3mf`: 2 ms locally, 3 ms on the Intel Mac.
- In Studio, opening that source succeeded, returning to the library enabled
  Print again, and the next item's context menu opened normally.
- Disposable GUI checks assigned slot 3 to a fresh model and slot 4 to one part
  of the multipart Boy Mom project. The selected part retained 4 while its
  sibling retained 2 after inline editors closed. Names stayed intact. No print
  was sent. Temporary key-event diagnostic code was removed from the release.
- Both release builds completed. The signed Intel bundle passed the macOS 12.6
  architecture, dependency and Apple signing audit. Final assistant layout was
  inspected locally; no live Intel Mac UI session was launched over SSH.

## Installed releases

Both machines use `/Applications/BambuStudio.app`, with previous apps preserved
for rollback. Executable SHA-256:

- ARM: `9a3c207b833dda88cd086bec0c4ad01bfbd5487ee032e97ef57b5eb9e1a68f58`
- Intel: `e5c61fb48054122a524367115a6bacee38a8f014b0123a364e6547257cbe7c88`

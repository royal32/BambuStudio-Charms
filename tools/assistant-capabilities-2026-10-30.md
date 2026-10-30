# Studio assistant capability expansion — 2026-10-30

The assistant now exposes **64 native tools**, up from 33. This substantially
expands ordinary modeling and print preparation; it is **not complete parity
with every interactive Studio control**.

## Available workflows

| Area | Direct assistant controls |
| --- | --- |
| Understand the model | Fresh colored model views, numbered part maps, measured surface patches and numbered surface maps, physical sections, per-instance transforms/bounds, current warnings and overrides. |
| Supports and painting | Enforce/block supports or seams, assign surface colors, paint fuzzy masks, clear one annotation layer, and clip painting to a measured spherical brush. Preserve other faces and annotation layers. |
| Geometry | Capped plane cuts, part removal, positive/negative primitives, settings/support modifier volumes, standalone primitives, native font lettering/engraving, union/subtraction/intersection, and native simplification. |
| Position and combine | Explicit instance/part translations, rotations, nonuniform resizing, mirrors, bed/face placement, align/distribute/center/fit, split/assemble objects, and detach linked copies before independent edits. |
| Print preparation | Discover actual process keys and valid scopes; set global/object/part/plate values; restore inherited overrides; choose compatible profiles; inspect safe printer/material parameters and cached printer/AMS data; slice one/all plates and inspect actual sliced solids, holes and support/tool paths. |
| Files and library | Open/import an identified local model, search and inspect library sources, import assembled models, save/export through native workflows, publish sliced plates with their editable project/vector previews, and open human print review. |

Numbered diagrams identify geometry; their colors are not filament colors.
They deliberately do not pretend to subtract negative volumes. Sections and
actual sliced layers provide that evidence. Hidden parts do not bleed through
the numbered maps, and labels are anchored on visible pixels.

## Behavioral changes

- Use measured IDs, a numbered part view when needed, and visible evidence
  before changing a visually described feature. Verify the requested result,
  surrounding geometry, and physical dimensions afterward.
- Infer reversible defaults and carry out the task. Adding a new digital
  filament slot with a compatible profile/color no longer needs an additional
  confirmation when ordinary edits are allowed. Existing shared profile
  changes retain their confirmation.
- Prefer direct tools to opening an interactive editor. Support paint alone
  does not prove a successful fix: enable suitable settings and inspect real
  generated support layers.
- The fresh context no longer contradicts the tool list by claiming that
  surface painting is unavailable. Assembly mode is reported explicitly; switch
  to Prepare and inspect its synchronized state before editing.
- Do not save, publish or open extra file dialogs merely as a completion step.
  Native cancellation is honored. An uncertain load/export cannot repeatedly
  reopen its workflow in the same request, including with a new revision.
- Library search results are bound to their selected folder, including during
  asynchronous reads. Changing folders invalidates an earlier result.
- Treat reported AMS colors/materials as cached configuration, including the
  family's placeholder white slots; they are not proof of the actual spools.

Modeling work is staged, canceled/discarded without applying stale results,
and committed with native Undo snapshots. Boolean operations preserve physical
dimensions under rotated/mirrored/nonuniform instances, use a local computation
origin for distant plates, and refresh bounds after discarding unused cutter
vertices. Invalid coordinates are rejected before surface rasterization.

## Remaining boundaries

Detailed cut-connector setup, conformal/per-glyph text and SVG editor controls,
variable-layer-height editing, arbitrary filament/printer profile editing,
some advanced repair/editor operations, and several library/device-management
commands still need their native UI. Selecting profiles and reading ordinary
material/printer parameters are available. Opening an editor is explicitly not
reported as completing an edit. No shell, account administration, raw custom
G-code, or unattended final print submission is exposed.

Bounded native work also has limits: surface/section work and simplification
cover at most two million source triangles; booleans cover at most one million
combined triangles. Larger sources can require native interactive preparation.

## Verification

- ARM build and eight native suites passed: tool/protocol contracts, surfaces,
  transforms, mesh edits, text, worker cancellation, selection/load state, and
  the scripted fixture driver. Final numbered-piece depth masking and invalid
  coordinate checks passed.
- Six Intel suites passed on an **actual macOS 12.6 / x86_64 Intel Mac**, then
  passed again with the final numbered-piece/invalid-coordinate surface checks.
- Three scripted workflows passed physical assertions: support enforcement on
  the 396 mm² intended underside without changing geometry/placement; loop
  removal plus a 2 mm bore checked at three depths; and a 30×20×3 mm keychain
  with a 3 mm through-hole and centered, raised 5 mm lettering.
- One initial native Boolean test failed during heavy concurrent build/test
  activity. A controlled 60-run concurrent follow-up passed, as did subsequent
  ARM/Intel suites. The native backend can still reject geometry; errors leave
  the staged project changes unapplied. This is not evidence that all inputs
  are repairable.
- The Intel review bundle passed architecture, dependency, deployment-target
  and signature audits for macOS 12.6.
- The ARM build runs on the current macOS 27.0.1 development Mac and retains
  its existing Homebrew zstd/Imath runtime dependencies. Its portability audit
  therefore does not pass as a standalone redistributable ARM package; the
  separately built Intel package has no such external dependencies.
- Synthetic Keychain checks passed: legacy migration preserves its original
  encrypted item, new signed storage remains accessible across changed binary
  bytes/paths with the same signing identity, foreign identities fail, and
  sign-out removes both records.

The initial results above are native/scripted tests, **not Sol/Luna performance
results** or a claim that the complete GUI was exercised. The initial local
credential check was blocked by a locked Mac and protected approval. The Intel test machine's
older Studio was running; its login/session was left untouched. Optional account tests
must run with Studio closed so old/new credential stores cannot refresh the
same account independently. No print was sent.

## Reproducing the disposable checks

```sh
python3 tools/run_studio_assistant_model_checks.py \
  --test-source tools/assistant_workbench_eval_checks.cpp \
  --check-name assistant-workbench-fixture-checks
```

The fixture driver creates its own geometry/configuration and sends no network
requests. `tools/assistant_live_client_check.cpp` is a separate opt-in developer
diagnostic; normal account/evaluation mode refuses to run alongside Studio.
Its metadata mode reads no credential values. Optional real-model cases have a
five-minute deadline and record native assertions, calls, results and views.
The headless fixture dispatcher covers a subset of the app: a valid tool it
does not implement marks the trial inconclusive/comparison-ineligible, rather
than being treated as a model capability failure. No model weights were trained.

The 64-tool ARM app is installed at `/Applications/BambuStudio.app`; previous
apps are preserved in the dated `Installed-Backup*` directories under
`build/arm64`.

Follow-up: a late approval reply arrived after the original three-minute
request had timed out; a fresh three-minute request also expired. The explicit
approval window is now fifteen minutes and Stop interrupts/reaps its private
helper. Background access remains noninteractive with its ten-second bound.
Disposable credential/signature/cancellation checks and the client contract
suite pass. After the user unlocked/quit Studio, the fixed app was installed.
The first real approval exposed an additional login-Keychain partition check:
the self-signed app could restart, but different bytes with the same designated
requirement still needed approval. The disposable custom-Keychain test had not
covered that login-Keychain restriction.

The signer now supports an explicitly selected existing Apple Development
identity, verifies its Apple trust/Team ID, pins its exact certificate and refuses
to fall back to a different identity. Both architectures use that identity.
An additional UUID-named synthetic login-Keychain test verifies access across
different executable bytes/paths, denies foreign signatures and same-team wrong
app identifiers, and verifies removal of only the synthetic item. It never
locks/unlocks/deletes the login Keychain or reads real account credentials.
The distribution audit now rejects self-signed packages without an Apple Team ID.

The Apple-signed app is installed locally. The user approved its Documents
access and explicit assistant credential migration. The GUI connected, then a
separate, differently built Apple-signed executable successfully fetched the
real model catalog without another authorization prompt. This verifies real
cross-build access, in addition to the synthetic regressions.

The Intel test machine's signed/audited review is staged at
`~/Downloads/Charms-Assistant-Capabilities-20261030/BambuStudio.app`. Its executable
is hash-checked against the local Intel artifact. The working Applications app
is preserved while that review awaits a normal desktop-session check after
quitting/saving the current Studio session. No authenticated GUI was launched
over SSH. Final hashes/runtime summaries are in the build reports.

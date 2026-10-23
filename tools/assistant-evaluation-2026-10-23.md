# Studio assistant comparison — 2026-10-23

Small paired check requested: GPT-6.1 Sol at its existing default effort versus
GPT-6 Luna at explicit `reasoning.effort: max`. Use the same instructions and
native tools, fresh conversations, and a fresh baseline for each model/task.
No printing or edits to shared source projects.

## Completed live run

**Sol / Namaste: pass.** Prompt: “Please make all the internal pieces a different
color.” Baseline: local `build/assistant-fixtures/Namaste Part Colors Before.3mf`,
with 15 assembled parts and existing white/black compatible filament slots.

The assistant inspected the project, assigned the inner parts, and sliced. It
chose black without asking a color-preference question. Its complete reply:

> All the internal pieces are now black, while the outer/base piece stays white.
> I resliced the plate, and Studio reported no warnings.

Saved disposable result: `build/assistant-fixtures/Eval Sol Namaste.3mf`.
Archive verification confirms fourteen parts use black slot 2 and one uses
white slot 1. Mesh vertices/triangles, component and instance transforms,
printer/process/filament IDs, and project slot colors match the baseline.

## Remaining paired checks

| Task | Exact request | Pass criteria |
| --- | --- | --- |
| Inner colors | Please make all the internal pieces a different color | Choose compatible contrast without asking a preference; keep the white base, geometry and profiles; assign 14 inner parts; slice and inspect the result. |
| Copies and placement | Make two more copies and arrange them on this plate, then check the slice | Three complete Band Mom charms, with assemblies/color assignments intact, within the A1 mini plate; valid slice and accurate warning report. Start from `build/assistant-fixtures/Band Mom.3mf`. |
| Supports | The top is unsupported. Please add suitable supports and check it | Inspect geometry/diagnostics and actual setting definitions; enable appropriate supports, preserve profiles and placement, reslice and report remaining concerns. Use the synthetic `Unsupported overhang.stl` emitted by the native model checks with the same A1 mini/PLA setup for both models. |

The remaining Sol runs and all Luna runs are **pending**, so there is no evidence
yet to recommend replacing Sol with Luna. After the first run, restarting the
rebuilt ad-hoc-signed app required macOS Keychain authorization. Stack sampling
confirmed the worker waiting in `wxSecretStore::Load` /
`SecKeychainFindGenericPassword`, rather than in inference. Computer Use cannot
operate that protected system dialog; a request for the owner's authorization
was sent. Do not count that wait as model latency or an inference failure.

The request-shape test verifies that Luna sends Max effort and Sol keeps its
existing default. Client/session and native geometry/slicer checks pass,
including real unsupported-overhang detection followed by support generation
and a fresh slice. These checks establish tool behavior, not Luna's ability to
choose those tools; they are not substitutes for the pending live comparison.

## Keychain fix before continuing

The owner requested that repeated credential prompts be fixed before further
model trials. The old app's ad-hoc designated requirement pinned its changing
executable hash, so a rebuild invalidated its previous Keychain authorization.
Staged ARM and Intel packages now use one persistent, certificate-pinned private
build identity. Background credential operations disable authorization UI in an
isolated same-executable child; only the explicit account-menu authorization
action can permit a prompt. No real account credential was accessed in testing.

Synthetic native checks pass on Apple Silicon and the actual macOS 12.6 Intel
Intel Mac: restarts, moved/changed executable with the same certificate, atomic
replacement, locked-Keychain rejection, and foreign-signature rejection for
read/update/delete. Private pipe checks also verify the signed parent, invalid
service rejection, host child-reaping behavior, and unchanged parent interaction
policy. Disposable test Keychains are removed automatically. An initial test
using only per-query UI suppression unexpectedly opened an OS prompt; that
disposable Keychain was removed and the test corrected to disable legacy UI
process-wide before accessing it.

The currently approved installed apps are deliberately untouched while the
owner is away. Adopting the stable certificate requires one final explicit
authorization of the old credential with **Always Allow**; it cannot inherit
the old ad-hoc authorization securely. No automatic authorization, model trials,
or real account sign-in checks are run while this changeover is pending.

## Further experience checks

The follow-up work was assisted by two GPT-6.1 Sol High reviewers and a GPT-6
Luna quota watcher. No reset credit is consumed automatically. Live model
comparison remains pending explicit authorization of the certificate-signed
review app; synthetic tests do not count as model capability trials.

Changes include Stop/Escape cancellation and its worker wake-up race, suppression
of late streamed/completion events after Stop, preserved drafts when using quick
actions, retained text selection/focus, cached transcript layout, and adjacent
streamed-text coalescing. Failed or declined preplanned action batches stop, and
a declined tool cannot reopen its dialog in the same user turn. Async library
failures no longer receive a completed tick.

Native object support edits preserve placement and reject global-only keys that
would have no effect as object overrides. Selection edits bind Prepare's inspected
selection even after Preview. Library search is deterministic, bounded to 40
retained names, and reports truncation/total matches so the model can refine it.

Connection failures receive short recovery messages with separately copied
HTTP/code/parameter/request-ID diagnostics. Synthetic checks cover nonstandard
admission responses, temporary failures, usage limits, malformed and EOF-ended
streams, and Unicode/control-byte diagnostics. Raw provider/parser fragments
are not put into chat or copied diagnostics. There are no automatic inference
retries, billing changes, or OAuth loops.
Model-list recovery distinguishes transient retry from explicit sign-in for
missing/expired credentials. A persistent deferred refresh fixes separate-tick
sign-in completion, honors cancellation, and consumes the follow-up once.
Import/export outcomes are explicitly unconfirmed when native dialogs provide
no success signal. The session checks results before continuing a preplanned
batch and prevents identical document-action dialog retries during that turn,
while permitting inspection and different document actions. This avoids false
import/export success and repeated choosers after cancellation.
Native project opens follow the same rule: a selected filename or close-dialog
return code does not establish loading success. Identical library-source opens
also cannot repeat within that turn. Selection revisions include the exact
selected volumes and selection mode, so switching between parts in one object
invalidates a previously inspected revision before an edit.
Identical failed native mutations are skipped within the turn, keyed by the
validated arguments and normalized tool name. Changed arguments/revisions and
new user turns remain usable; successful repeats, inspection and slicing keep
their existing behavior. The pre-fix negative control executes an unchanged
failing arrangement 32 times; the regression now permits one native attempt.

Validation completed before adoption:

- ARM and Intel Studio builds and strict persistent signature checks pass; the
  final Intel bundle audit reports no macOS 12.6 compatibility errors.
- Pure client/session, worker race/order, library-search and selection/result-state
  targets pass locally. All four targets also pass on the macOS 12.6 Intel Mac.
- Real native chat-widget checks pass locally and on the Intel Mac, covering light/dark
  contrast, narrow/wide wrapping, selection retention, action-only replies and
  streaming growth. Font changes invalidate cached heights; selected text in an
  actively streaming reply survives appended chunks without selecting new text.
  Long filenames, URLs and unbroken Unicode no longer receive a one-line height.
  A separate native Cocoa layout probe fits all 30 default/larger-font cases
  on both this Mac and the macOS 12.6 Intel Mac;
  the portable widget fixture covers the same input families without changing
  displayed or copied text.
  CUA visual inspection confirms readability and selected-text
  versus whole-message copy menu items on the Apple Silicon Mac.
- The old lock-free worker Stop setter fails the new concurrency regression in
  a disposable negative-control build; the fixed implementation passes.
- Real slicer checks generate object-only supports with global supports disabled
  and clear the unsupported-overhang warning. Five project fixtures (Band Mom,
  Wanda, Frappe, Bee, Namaste) plus the raw Namaste STL pass geometry/material
  checks; input SHA-256 values remain unchanged.
- The signed full ARM app and full Intel app on the Intel Mac pass their invalid-service
  private credential transport self-checks. The restricted Codex
  sandbox blocks macOS signature inspection, so it cannot run that native check.

The final app remains staged separately from the working installed apps. The Windows test machine
did not answer an SSH connection attempt, so this follow-up has no fresh Windows
build or live Windows UI validation.

Review artifacts (code through `fd5a9f224`):

- ARM: `build/arm64/Assistant-Experience-Ready/BambuStudio.app`.
- Intel archive: `build/x86_64/BambuStudio-Charms-Intel-macOS12.zip`, SHA-256
  `dad5e8ea20f81d090c2a6a7df51e5c325e6df83ad030e62828d33f9b62b3dd00`.
- Intel Mac: `~/Downloads/Charms-Assistant-Experience-Final-2026-10-23/BambuStudio.app`.
  Its archive hash, strict signature, startup and private credential transport
  were checked on the Intel Mac without launching its GUI or reading an account.

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

# Studio assistant

Click the **sparkle button** beside the slicing controls, or open
**Help → Studio assistant…**. Choose **Continue with ChatGPT**, finish the
browser sign-in, and authorize ChatGPT plan usage for **Charms Studio Assistant**.
The model menu lists the models available to that account. **GPT-6 Luna (Max)**
explicitly uses maximum reasoning effort; other models retain their defaults.
The **⋯** menu contains account selection, sign-in/out, **New conversation**,
and the option to confirm every edit. **Manage usage** in that menu opens
ChatGPT's usage settings. This uses the connected ChatGPT plan, not a separately
billed API key. Availability and allowance depend on that account.

Try requests such as:

- “Explain the warnings on this plate. Which ones matter?”
- “This earring loop is unsupported. Inspect it, suggest a fix, then slice to check it.”
- “Make three copies and arrange them on this plate.”
- “Keep the largest outer shell white and make the smaller internal pieces black.”
- “Find the Bee in the library and add it to this plate, keeping its model colors.”

The assistant can inspect objects, parts, plate and object overrides, profiles,
current slicing warnings, and model views. It can change supported process
settings, choose installed printer/process/filament profiles, select objects,
copy, transform, arrange or orient parts, move them between plates, and slice.
It can highlight individual parts and assign their project filament slots. For
an STL with disconnected pieces, it can use **Split to parts** to expose those
pieces while keeping them assembled in the same object. It inspects each part's
name, dimensions, bounds, effective filament and existing color painting.
Assigning a part affects all instances of that object, while leaving other parts
and material profiles unchanged. Existing color painting is preserved unless a
person explicitly confirms replacing it with a uniform color. Negative volumes
and modifiers cannot be assigned using the part-color tool.

Changing a project filament's display color is a separate operation with a
confirmation, because every part using that slot changes appearance. Physical
AMS settings remain unchanged. For a color needed on only some parts, the
assistant reuses an appropriate slot or adds one with a compatible profile.

It can search the sliced library and import editable source models while
preserving painted colors, support annotations and negative volumes. If the
destination lacks matching material/color slots, it asks for an explicit mapping
instead of silently recoloring the model.

Painting individual faces remains a manual operation: the assistant can open the
appropriate painting tool and guide the person. A clean slice is not a guarantee
that a part will print successfully. It cannot determine physically loaded
filament from the family's all-white AMS slot colors.

The chat uses message bubbles, with compact Studio action updates below the
assistant's reply. Text can be selected or copied. Enter sends a message;
Shift+Enter inserts a new line. Send becomes **Stop** while working. The two
shortcuts explain warnings or help with supports.

The assistant favors short, everyday language and takes sensible, reversible
steps without asking for every preference. For example, "make the internal
pieces a different color" reuses a contrasting compatible project color and
reports its choice. It still asks when identifying the pieces is ambiguous or
a physical material choice matters, and preserves existing color painting.

Project edits use Studio's Undo history. Enable **⋯ → Confirm every project edit**
to review each edit before it runs. Saving, opening another project, selecting
profiles, and deleting parts have confirmation dialogs. File operations use
Studio's normal dialogs. **Stop** cancels further assistant work; an operation
Studio has already started may finish. The assistant does not send print jobs.

## Account and data handling

Each local installation has its own app registration. Multiple ChatGPT accounts
can be added and selected independently; switching accounts resets the
conversation. Access, refresh and ID tokens are stored only in the operating
system's credential store (Keychain on macOS, Credential Manager on Windows).
Refreshes are serialized across Studio processes. Sign out removes local
credentials and attempts to revoke the remote refresh token.
An active credential is cached in memory while valid, so individual model/tool
calls do not repeatedly access Keychain. It is reread under the process lock
when renewal is due, in case another Studio process has already renewed it.

Mac packages must use the persistent private-build signing certificate via
`tools/sign_macos_app.py`; the bundle audit rejects ad-hoc signatures and
identifier-only requirements. The first changeover from an old ad-hoc build
needs one explicit **⋯ → Authorize Keychain access…** action. Choose
**Always Allow** in macOS's dialog. Ordinary rebuilds signed with the same
certificate keep the same designated requirement and reuse that approval.
Changing the certificate, revoking access, or locking the Keychain can still
require a person's action; the app does not bypass those protections.

Automatic Mac credential reads, refresh writes and deletion never authorize UI.
An isolated same-executable child disables legacy Keychain interaction for its
own process only, so the network plugin and Connect-related code in Studio are
unaffected. The child validates its parent's executing code signature and uses
inherited anonymous pipes; credential values are never arguments, environment
variables, log output, or temporary files. It only accepts the assistant's
hashed service names. Reads/writes/deletes fail promptly when access is missing.
Only the explicit authorization action permits an OS dialog. The per-query
`kSecUseAuthenticationUIFail` flag alone is insufficient for legacy file-based
Keychain items on some macOS releases; it was not used as the sole protection.

Project context is sent to OpenAI when a person sends a request. That can include
object names, settings, warnings, library search results and requested model
images. Conversation context is kept in memory for this window; model requests
use `store: false`. Closing the assistant stops its current task. New conversation
clears its context without changing the project. ChatGPT conversations and memory
are not imported.

## Development checks

The assistant is native C++/wxWidgets; it requires no Python or Node runtime in
the distributed app. Its authenticated inference client uses the official Sign
in with ChatGPT token-sharing flow and the Responses API with namespaced tools.
No arbitrary shell/code execution tool is exposed.

Pure client/session checks:

```sh
cmake -S tests/assistant -B build/assistant-checks -DCMAKE_PREFIX_PATH=<built-deps-prefix>
cmake --build build/assistant-checks
ctest --test-dir build/assistant-checks --output-on-failure
```

Opt-in native Keychain checks use synthetic values and automatically delete
their disposable Keychain. Initialize the stable build identity once first:

```sh
python3 tools/sign_macos_app.py init
python3 tools/run_studio_assistant_keychain_checks.py
```

These checks cover process restarts, moved and changed executables signed with
the same certificate, locked-Keychain rejection, foreign-signature rejection
for reads/writes/deletes, and private credential-pipe/caller validation. They
never read an actual account credential. `tests/run_keychain_checks_macos.sh`
runs the prebuilt Intel probes on the macOS 12 Intel Mac without Python or CLT.
Both native checks pass with atomic executable replacement; on macOS 12,
overwriting an existing executable in place can leave stale identity state.

Native model/slicer checks, after building Studio with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`:

```sh
python3 tools/run_studio_assistant_model_checks.py path/to/fixture.3mf ...
```

The runner uses the existing app's compiler/linker configuration. It checks
painted-color swaps, support masks, negative volumes, clone identities, material
mapping, and the real slicer's unsupported-overhang/support-generation warning
lifecycle, then loads the supplied fixtures without changing them. Live testing
must also cover browser authorization, credential persistence, an actual model
request, native edits/reslicing and Undo. Never log OAuth URLs, callbacks, tokens,
or raw authenticated HTTP traffic while diagnosing sign-in.

## Validation, 2026-10-22

Apple Silicon and Intel builds pass. The Intel package passes the macOS 12.6
bundle audit and strict signature verification; native client/session tests and
the executable's command-line startup check pass on the 2017 Intel Mac. The tester also
confirmed that the assistant is readable in the Intel Mac's light mode, signs in,
answers a warning request, and does not repeatedly prompt for Keychain access.

Live Apple Silicon checks cover saved ChatGPT sign-in after restart, an actual
model request, support-setting inspection/change and reslicing, copying from
Preview into Prepare, arranging and slicing three complete Band Mom charms,
Undo back to one charm, and a rendered-model image returned to the model.
Stale revisions are rejected before editing. Stop returns control promptly
while a source lookup is waiting for iCloud. No test changed shared source
projects or sent a print job.

The native model checks pass with Band Mom, Bee, Wanda, Larkin and Frappe source
fixtures, plus synthetic painted-color swaps, negative volumes, support masks
and distinct assembly identities. All five copied fixtures retain their original
hashes. The credential-cache test verifies that 100 successive calls cause one
credential-store read, that accounts remain separate, and that renewal rereads
the latest saved token and sign-out clears the cache.

The live shared-library search succeeds, but its source lookup encountered
undownloaded iCloud project-link records and timed out before import. Import
geometry/material handling is covered by native tests; that particular live
iCloud import remains unverified. Make the shared library's
`.charm-library/projects` metadata available locally when source lookup waits on
iCloud. The existing library source resolver reads those records on a worker;
the assistant's five-minute wait limit leaves the project unchanged on timeout.

Part-color refinement: the assistant assigned Namaste's 14 inner pieces to the
existing black slot and retained its largest outer shell on the white slot,
then sliced successfully with no reported warnings. A saved local test copy has
one part assigned to slot 1 and fourteen to slot 2. Comparing its 3MF with the
before copy confirms identical mesh vertices/triangles, component and instance
transforms, printer/process/filament profile IDs, and project slot colors. The
original STL is unchanged. Native checks also split the raw Namaste STL into
15 parts without changing its bounds, and exercise invalid slots, wrong-object
part IDs, negative-volume rejection, and explicit replacement of existing color
painting while retaining support painting and other parts' assignments.

## Chat refinement, 2026-10-23

Apple Silicon and Intel builds, client/session checks (including explicit Luna
Max request settings), and native model/slicer checks pass. The packaged Intel
app passes strict signature verification and the macOS 12.6 dependency audit.
The local sparkle launcher, compact menu, chat alignment and wrapped text have
been checked in the native UI. The Intel update is staged separately because
The Intel Mac's current review app is still running.

The first live Sol run chooses black for Namaste's inner pieces without asking
an unnecessary preference question, then slices successfully. The saved test
copy preserves geometry, transforms and profiles. The remaining Sol/Luna
comparison needs macOS Keychain authorization for the rebuilt app; results and
the exact remaining checks are recorded in
[the comparison notes](assistant-evaluation-2026-10-23.md). Sol remains selected.

Official references: [sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in),
[profiles and sessions](https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions),
[models and inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference),
[Luna reasoning settings](https://developers.openai.com/api/docs/models/gpt-6-luna).

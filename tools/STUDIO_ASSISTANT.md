# Studio assistant

Open **Help → Studio assistant…**. Choose **Continue with ChatGPT**, finish the
browser sign-in, and authorize ChatGPT plan usage for **Charms Studio Assistant**.
The model menu lists the models available to that account. **Manage usage** opens
ChatGPT's usage settings. This uses the connected ChatGPT plan, not a separately
billed API key. Availability and allowance depend on that account.

Try requests such as:

- “Explain the warnings on this plate. Which ones matter?”
- “This earring loop is unsupported. Inspect it, suggest a fix, then slice to check it.”
- “Make three copies and arrange them on this plate.”
- “Find the Bee in the library and add it to this plate, keeping its model colors.”

The assistant can inspect objects, parts, plate and object overrides, profiles,
current slicing warnings, and model views. It can change supported process
settings, choose installed printer/process/filament profiles, select objects,
copy, transform, arrange or orient parts, move them between plates, and slice.
It can search the sliced library and import editable source models while
preserving painted colors, support annotations and negative volumes. If the
destination lacks matching material/color slots, it asks for an explicit mapping
instead of silently recoloring the model.

Painting individual faces remains a manual operation: the assistant can open the
appropriate painting tool and guide the person. A clean slice is not a guarantee
that a part will print successfully. It cannot determine physically loaded
filament from the family's all-white AMS slot colors.

Project edits use Studio's Undo history. Uncheck **Allow changes to this project**
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
while a source lookup is waiting for iCloud. No test saved a project or sent a
print job.

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

Official references: [sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in),
[profiles and sessions](https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions),
[models and inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference).

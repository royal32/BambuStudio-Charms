# Printing with Bambu Connect

On macOS, **Print plate** opens Studio's printer/options dialog. Choose the
printer, print options and filament mapping, then **Send**. Studio exports the
job and passes those selections to Connect's internal print state. Connect runs
hidden during a normal send. Its existing Send handler performs submission.
Studio's print dialog closes after the handoff completes, including when a
**Continue printing** warning was confirmed first.

**Open in Bambu Connect** in the Studio dialog is the review/fallback path. It
opens Connect directly at **Send to print**, skipping both **Import Gcode 3MF**
and the preview's **Print** button. It can also be used when Studio's printer
status prevents its Send button from becoming available. **Print all** and
multi-printer dispatch open this Connect setup path; they do not automatically
start several jobs. Select the desired plate/printer there.

For a direct toolbar fallback, choose **Print in Connect** from the print-button
dropdown. Clicking that toolbar button then opens Connect's send dialog directly.

If Connect rejects the chosen printer, requires a filament warning to be
confirmed, changes an option because of printer capabilities, or cannot reproduce
a mapping, it opens for review. Multi-extruder and manual-change-assistance jobs
also require Connect review until their full option semantics are verified.

On macOS, Studio keeps Connect on the selected Studio account automatically.
Use the account menu at the upper left of **Home** to add or select an account.
Adding an account restarts Studio and opens its normal sign-in page. After that,
selecting a saved account restarts Studio with its saved login and switches
Connect in the background; a separate Connect login is unnecessary. Studio's
normal save/cancel prompt still applies to an open project before restarting.

Each account has a separate Studio data directory for its session, printer
access codes and presets. New accounts inherit the installed network plugin and
basic application/printer setup so they do not repeat the first-use wizard.
Logging out also signs Connect out. A failed account sync blocks Connect handoffs
until the accounts match. If using a mainland China account, select that region
in both apps first. Account synchronization currently requires the supported
macOS Connect version listed below.

On the Device tab, **Open Bambu Connect** opens the printer-control application.
Select the printer there to move axes, change temperatures or operate the AMS.
If an existing Studio control is rejected as unsigned, the dialog now explains
that the action was not performed and offers to open Connect. It does not replay
the rejected command. Other direct workflows, including Send to printer storage,
reprinting a file already on the printer, and calibration, still use the original network plugin and may remain restricted
by firmware.

## Direct bridge (macOS, Connect 2.5.0-beta.15)

The first direct handoff gracefully restarts an already-running Connect so the
signed executable can inherit two private debugging pipes. Subsequent handoffs
reuse that process. Studio now starts hiding that process as soon as it registers
with macOS, and maintains the hidden state throughout initialization, instead of
waiting for the renderer to finish loading. This does not modify Connect's app bundle, persist debugging
settings, or open a TCP listener. The pipe connection belongs to the Studio
session; closing Studio can also close that Connect instance. Printer-side jobs
already started continue independently.

`BambuConnectBridge.mm` launches Connect with `--remote-debugging-pipe`, locates
the renderer, and retrieves its print-store accessors from the module scope.
`resources/scripts/bambu_connect_bridge.js` uses those methods to import the
sliced file, select its plate/device, set options and map filament trays.
The bridge reads the current account ID and access token from Studio's existing
network plugin in memory. Its public login-info API contains only display
metadata, so the session reader is pinned to the installed arm64 plugin's Mach-O
UUID `22253083-6232-325A-87FB-ED9092625F5E`. It checks that binary identity before
using the verified account/string offsets, bounds memory reads, and checks the
account ID before and after reading. An unsupported binary or inconsistent
snapshot returns no token. Update this adapter when the network plugin changes;
do not remove the UUID guard. This part of the integration currently supports
Apple Silicon only.

The bridge uses Connect's own secure-storage writer to select
that session, then restarts Connect to discard old MQTT connections and printer
caches. Tokens are never placed in job files, the account registry, URLs,
command-line arguments or bridge logs; no additional password store is created.
Connect remains responsible for authentication, upload and printer communication.
The selected account ID is checked again before importing and before submission.

The adapter locates the current React send modal and invokes its existing `onOk`
handler only when the modal is enabled and the requested selections still match.
It never calls `_print` directly or automatically confirms a warning. Each
prepared request may be submitted once. An uncertain submission result opens
Connect for inspection and is not automatically retried.

This is intentionally an unsupported, version-specific integration. The host
checks the Connect version and expected renderer modules. To update it, inspect
the new renderer's store API and modal props, update the module names/store
bindings and adapter together, then rerun the tests and a prepare-only live check.
Do not merely remove the version check. If establishing the print bridge fails,
Studio can fall back to the standard URL importer only after account verification
succeeds. If the account interface also changed, update the adapter first or
export a sliced file and import it manually in Connect.

## Standard URL fallback

The [official Connect documentation](https://wiki.bambulab.com/en/software/bambu-connect)
(checked September 13, 2026; installed Connect 2.5.0-beta.15) specifies
`bambu-connect://import-file` with individually URL-encoded absolute `path`,
display `name`, and `version=1.0.0`. It does not document remote axis commands,
printer preselection, filament-mapping parameters, silent submission, or an
import-completion callback. A successful app launch is not a successful print.

Connect 2.5.0-beta.15 decodes the complete query before decoding individual
parameters. The handoff compensates with an extra encoding pass on both values;
this was verified against its public URL receiver and with a real import. The
standard encoding remains available in the helper for a future Connect version
that fixes this behavior. Recheck this compatibility workaround when upgrading
Connect. Its import confirmation is part of Connect's UI.

The standard fallback requires Connect's import confirmation and preview Print
button. On macOS it addresses `com.bambulab.bambu-connect` using `/usr/bin/open`
with separate arguments. Other platforms retain the OS URL-handler integration.

Exports use the existing sliced-3MF exporter, including this fork's thumbnail
view-angle customization. Each handoff gets a unique `.gcode.3mf` in the Studio
data directory's `bambu-connect` subdirectory, independent of live slicing files.
Snapshots survive Studio closing and are removed on a later handoff once seven
days old. Reopen a week-old job from Studio instead of relying on Connect's old
import. A failed export never launches Connect; a failed launch offers install
and sign-in guidance. Manual sliced-file export remains available.

## Validation

Baseline verified September 13, 2026, committed as `7cedc3d7c`:

- The full arm64 build passed with
  `CMAKE_BUILD_PARALLEL_LEVEL=4 ./BuildMac.sh -s -x -b -a arm64`.
- All three `[bambu_connect]` Catch2 tests passed (six assertions), covering
  the vendor URL example, reserved query characters, Unicode, Windows paths,
  and the installed beta's extra decoding pass.
- A newly sliced plate and an imported sliced 3MF both opened in Connect with
  the expected thumbnail and print estimates. A filename containing `café & #1`
  survived the handoff. The generated archive contained G-code and thumbnails.
- Connect's final setup offered printer selection and filament mapping. It
  correctly disabled sending an A1-profile job to A1 Four, which is an A1 mini.
- The Device tab's Open Bambu Connect button launched Connect.
- Print all exported both test plates in one valid archive. Connect displayed
  both thumbnails and separate print estimates when selecting each plate.
- One X+1 command through Connect on A1 Four produced movement, physically
  confirmed by the user. No test print was started.

Direct-bridge validation:

- The final arm64 build passed with the same `BuildMac.sh` command above.
- The startup-hiding change passed the arm64 build. A fresh native launch
  completed with macOS reporting Connect hidden; explicitly opening it afterward
  worked. A subsequent prepare-only handoff opened the final review dialog with
  plate 2, A1 Four and the requested options/mapping intact. No print was sent
  during these startup checks. The user confirmed a brief flash remains but
  is fast enough to be insignificant.
- Ten adapter tests pass with
  `node --experimental-vm-modules --test tests/bambu_connect_bridge.test.cjs`.
  They cover exact selections, changed/absent mappings, normalized options,
  Connect's disabled state, warnings, duplicate requests, bad imports and busy
  state, startup redirects and animation frames during hidden handoffs, and
  rejection when the active account differs before import or submission. Send
  dispatch is mocked in these tests.
  Closing the modal without Connect's success navigation is not reported as a
  successful submission.
- A compiled native bridge imported the test job directly into the Send to print
  dialog, selected A1 Four, and transferred bed-leveling/flow-calibration options
  and the external-spool mapping. A second handoff reused the existing process.
- The rebuilt Studio's **Open in Bambu Connect** button opened the final Send to
  print dialog directly with A1 Four selected. Bed leveling and flow calibration
  transferred with both On and Off selections across repeated handoffs. Original
  Studio option selections were restored afterward.
- A two-plate archive imported through the native bridge with plate 2 selected
  and its separate 23-minute, 5.19-gram estimate visible in Connect.
- A requested timelapse setting that Connect normalized was detected and marked
  for review. The A1/A1-mini compatibility check remained active.
- The user verified a live silent submission from Studio: the job arrived and
  started on the printer successfully. The subsequent dialog-close fix defers
  sending until the confirmed warning's modal loop has unwound; the user
  confirmed the lingering dialog occurred after **Continue printing**.
- The native dialog fallback is verified end to end. The custom toolbar
  dropdown's **Print in Connect** selection still needs a manual UI check.

For future regression checks, repeat these tests with the installed Connect
version and check a selected non-first plate, missing sliced data, and the
rejected-control dialog. Check that uncertain outcomes never automatically retry.

Account-switching checks:

- Browser sign-in must close and unwind the login modal before dispatching
  privacy, preset-sync, or Connect work. An independent delayed close can hide
  the login window while another modal is active; wxWidgets then refuses to
  exit the suspended loop and leaves Studio disabled. A sample of the reported
  freeze showed this orphaned login loop while the preset worker had already
  reached its periodic sleep. A native probe using the bundled wxWidgets
  reproduced the old ordering and passed with the corrected ordering. For a
  full login regression, add an account via browser sign-in, accept preset
  synchronization, then verify that Home, Prepare, and the account menu respond.
  The corrected arm64 build passed and reopened with the third account retained;
  its account menu and empty Prepare workspace responded. A fresh browser login
  has not yet been replayed against that build.
- The arm64 build passed with the same `BuildMac.sh` command above.
- Created the second profile from Studio's account menu and completed its normal
  Studio login. Connect then signed into that same account automatically and
  showed no bound printers. The first profile's printers did not remain cached.
- Repeated saved-account switches in both directions through the Home menu.
  Each restart preserved the selected Studio login and synchronized Connect
  without another sign-in. Returning to the main account restored its six
  printers, including A1 Four.
- After the round trip, Studio's print dialog handed a sliced test fixture
  directly to Connect's final dialog with A1 Four, bed leveling On, and flow
  calibration/timelapse Off. The expected printer-profile compatibility warning
  still disabled Send. No print was submitted during account-switching tests;
  Studio and Connect were left on the main account afterward.
- Five `[AccountProfiles]` cases pass (43 assertions), including canceled
  switches, isolated settings, and macOS framework symlinks.
- Three `[ConnectSession]` cases pass (10 assertions), including short/long
  strings, invalid pointers, oversized strings and unknown plugin binaries.
  On macOS, run them with:
  ```sh
  clang++ -std=c++17 -fobjc-arc -framework Cocoa -Isrc -Itests \
    tests/bambu_connect_session.test.mm -o /tmp/bambu-connect-session-tests
  /tmp/bambu-connect-session-tests '[ConnectSession]'
  ```

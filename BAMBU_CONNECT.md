# Printing with Bambu Connect

On macOS and Windows, **Print plate** opens Studio's printer/options dialog. Choose the
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

On Apple Silicon Macs, Studio keeps Connect on the selected Studio account automatically.
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

On Intel Macs and Windows, sign in to the same Bambu account in Studio and Connect, and choose
the same region in both apps. Studio verifies Connect's account ID before
importing a job and again before sending it. Account switching requires signing
in to that account in Connect too; automatic token transfer remains Apple Silicon-only.
The Mac plugin's private memory offsets are never used on Intel or Windows. Studio logout
also clears Connect's session when the bridge is available. A mismatched account
blocks the handoff instead of silently choosing a different account's printer.

On the Device tab, **Open Bambu Connect** opens the printer-control application.
Select the printer there to move axes, change temperatures or operate the AMS.
If an existing Studio control is rejected as unsigned, the dialog now explains
that the action was not performed and offers to open Connect. It does not replay
the rejected command. Other direct workflows, including Send to printer storage,
reprinting a file already on the printer, and calibration, still use the original network plugin and may remain restricted
by firmware.

## Direct bridge (macOS and Windows, Connect 2.5.0-beta.15)

The first direct handoff gracefully restarts an already-running Connect so the
signed executable can inherit two private debugging pipes. Subsequent handoffs
reuse that process. Studio starts hiding that process as soon as its native
windows appear, and maintains the hidden state throughout initialization, instead of
waiting for the renderer to finish loading. This does not modify Connect's app bundle, persist debugging
settings, or open a TCP listener. The pipe connection belongs to the Studio
session; closing Studio can also close that Connect instance. Printer-side jobs
already started continue independently.

`BambuConnectBridge.cpp` contains the shared protocol and handoff logic.
`BambuConnectBridge.mm` provides the Mac transport and session reader;
`BambuConnectBridgeWin.cpp` provides the Windows transport. Both launch Connect
with `--remote-debugging-pipe`, locate the renderer, and retrieve its print-store
accessors from the module scope. Windows passes its two inherited child handles
through Chromium's `--remote-debugging-io-pipes` switch. Its parent pipe endpoints
use overlapped I/O with deadlines and cancellation, a current-user-only DACL,
and remote-client rejection. `STARTUPINFOEX` restricts handle inheritance to the
two pipe endpoints. Startup and review use the owning process's windows; the
bridge requests a graceful close when restarting an existing Connect instance.
`resources/scripts/bambu_connect_bridge.js` uses those methods to import the
sliced file, select its plate/device, set options and map filament trays.
On Apple Silicon, the bridge reads the current account ID and access token from Studio's existing
network plugin in memory. Its public login-info API contains only display
metadata, so the session reader is pinned to the installed arm64 plugin's Mach-O
UUID `22253083-6232-325A-87FB-ED9092625F5E`. It checks that binary identity before
using the verified account/string offsets, bounds memory reads, and checks the
account ID before and after reading. An unsupported binary or inconsistent
snapshot returns no token. Update this adapter when the network plugin changes;
do not remove the UUID guard. This part of the integration currently supports
Apple Silicon only.

The official Windows and Mac 2.5.0-beta.15 packages contain identical renderer
bundles: `router-DcHfLi1L.js` (SHA-256
`94d0c7f63d743c077872243cbcdedf9fb11ef2d7ef0a8963e7b085183206427e`) and
`index-B_vXgx8V.js` (SHA-256
`080d5cf771de64a6c06795a543bb983a4a61549eeb4d1a2c44a65cfe623dbcf3`).

On Apple Silicon, the bridge uses Connect's own secure-storage writer to select
that session, then restarts Connect to discard old MQTT connections and printer
caches. Tokens are never placed in job files, the account registry, URLs,
command-line arguments or bridge logs; no additional password store is created.
Connect remains responsible for authentication, upload and printer communication.
The selected account ID is checked again before importing and before submission.

Connect can replace its renderer while keeping its main process running. Before
each new handoff, Studio validates the cached renderer and account-store handles.
Stale handles are rebound over the existing private pipe before importing a
file. On Intel and Windows, account verification also allows up to ten seconds
for Connect to restore its own saved sign-in after startup or a renderer reload.
A different signed-in account still blocks the handoff. Import and submission
failures are never automatically replayed.

The active Studio profile's `bambu-connect/bridge-events.jsonl` contains readable
handoff diagnostics: timestamp, status, phase, failure code and whether renderer
handles were rebound. It contains no request payload, account/session data,
device identifiers or raw protocol errors. This supplements the vendor-encrypted
Studio log. The standalone `bambu-connect-probe <adapter.js> --reload` check
reproduces invalidated account handles, verifies recovery and the signed-in
account match, and imports no files or sends any print.

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

Configure the full Windows application with `-DBBL_RELEASE_TO_PUBLIC=1`
and `-DBBL_INTERNAL_TESTING=0`, as `build_win.bat` and the Mac build do. The
source defaults are an internal build whose fresh profiles select Bambu's staging network service;
that service cannot provide the normal production network plugin or sign-in.
After installing into `install-windows`, run `tools\launch-windows-review.cmd`
to review the application with an isolated `test-profile` data directory.
The launcher uses the supported `--datadir` argument. Instance identification
already includes the executable path; there is no `--no-single-instance` option.

Windows bridge checks can be built independently of the full slicer dependency
build, from an x64 Visual Studio developer prompt:

```bat
cmake -S tests/connect -B build-connect-checks -G "Visual Studio 17 2022" -A x64
cmake --build build-connect-checks --config Release --parallel 4
ctest --test-dir build-connect-checks -C Release --output-on-failure
node --experimental-vm-modules --test tests/bambu_connect_bridge.test.cjs
build-connect-checks\Release\bambu-connect-probe.exe resources\scripts\bambu_connect_bridge.js
```

The native checks cover Windows handle inheritance, Unicode and NUL-delimited
pipe round trips, cancellation of timed-out I/O, recovery after cancellation,
disconnects, unverified session layouts, and import URL encoding. The probe
loads the real Connect adapter without importing a file or submitting a print.
Complete Connect's initial setup and sign-in before running that live probe.
To check real imports without sending, pass a JSON array of fixtures as the
probe's second argument and an optional printer display name as its third.
Each fixture contains an absolute UTF-8 `path`, display `name`, original
`plateIndex`, and a `filaments` array of the used one-based filament IDs.
The harness forces `submit=false` and never calls Connect's Send handler.
If MSBuild ignores an installed SDK's library paths, set `UseEnv=true` after
entering the developer prompt and configure with
`-DCMAKE_VS_GLOBALS=UseEnv=true`. This was needed on the Windows test machine.

The `charm-library` processing tool remains Mac-only. None of these Windows
targets include or invoke that tool.

Verified on the Windows test machine (Windows x64, MSVC 19.44), October 11, 2026:

- The full Release application and its dependency bundle build and install
  successfully. The packaged Studio DLL loads with its dependencies, exposes
  its entry point, and the production executable exits successfully for
  `--help --datadir "…\test-profile"` with the review launcher's arguments.
- The production network plugin 02.08.03.52 has a valid Bambu publisher
  signature and passes native DLL loading, Release ABI/version matching, and
  BambuSource dependency checks in the isolated Windows review profile.
  The user confirmed the corrected launcher opens Studio and sign-in works;
  the running application loads both network DLLs from that isolated profile.
- Native Windows pipe/URL checks and all ten renderer adapter tests pass.
- The signed official Connect 2.5.0-beta.15 application launches through the
  private pipes and passes the live renderer/store-binding probe.
- Prepare-only handoffs to A1 Five pass for Wanda Cartoon 2 Color (plate 5),
  Bee SB (plate 2), Aroma Joes Frappe (plate 5), Candy Cane SB (plate 3), and
  Starbucks Coffee Design 2 (plate 1). The harness checks the selected plate,
  bed leveling On, timelapse/flow calibration Off, AMS enabled and every used
  filament's slot mapping. The Wanda copy uses `测试 Prints/café & #1 …` in its
  Windows path. No print was submitted.
- The full arm64 Mac application and the three Mac session-reader cases
  (ten assertions) pass after extracting the shared runtime.

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
  clang++ -std=c++17 -fobjc-arc -framework Cocoa -framework Security -Isrc -Itests \
    tests/bambu_connect_session.test.mm -o /tmp/bambu-connect-session-tests
  /tmp/bambu-connect-session-tests '[ConnectSession]'
  ```
- The shared runtime lifecycle checks pass on Apple Silicon and natively on
  Intel macOS 12.6 (three cases, 30 assertions). They cover stale renderer
  handles, private-pipe reuse, transport failures, and no replay after failed
  preparation or uncertain submission. Run with CMake/CTest from `tests/connect`.
- On 2026-10-13, the Intel Mac's macOS log showed a renderer replacement between a
  successful morning print and later account-verification failures. A native
  forced-reload probe reproduced invalid account-store handles and verified
  rebinding/reuse over the same private pipe, without importing or printing.
  Authenticated verification must run in the normal desktop security session.
  An SSH launch could not restore Connect's Keychain credential, and Connect's
  subsequent unauthorized profile response cleared its saved login. Normal
  desktop quit/reopen preserved the login. The macOS transport now checks
  `SessionGetInfo` for graphical access before terminating or launching Connect;
  remote terminal sessions fail without touching the existing app or login.
  A GUI launch service may run the native `--reload` probe for a no-import,
  no-print authenticated recovery check. Its executable, adapter and output
  should be outside Desktop/Documents/Downloads to avoid folder-access prompts.
  `--account-state` reports only credential/account presence flags; it never
  returns credential values or changes the stored login.
- The 2026-10-13 authenticated Intel Mac desktop check passed: Connect restored its
  own login, a forced renderer reload invalidated the old handles, and fresh
  handles were rebound and reused with the same account. Two successive
  prepare-only handoffs (Band Mom and Bee) then verified their plates, options
  and live AMS mappings on A1 Five. No print was sent. The guarded native probe
  rejects an SSH launch before terminating Connect.

# Printing with Bambu Connect

This fork uses Bambu Connect for **Print plate**, **Print all**, multi-printer
print dispatch, and printing an opened sliced 3MF. Slice as usual, then click
**Print plate**. The sliced job opens
in Connect without a Save dialog or Studio printer-selection dialog. In Connect,
confirm **Import Gcode 3MF**, click **Print**, choose the printer and filament
mapping, then **Send**.
For a job containing several plates, select the desired plate in Connect first.
Connect owns the final print options and confirmation.

Keep Connect signed in to the same printer account used in Studio. The integration
does not change either login or transfer credentials. Account switching remains a
separate future improvement.

On the Device tab, **Open Bambu Connect** opens the printer-control application.
Select the printer there to move axes, change temperatures or operate the AMS.
If an existing Studio control is rejected as unsigned, the dialog now explains
that the action was not performed and offers to open Connect. It does not replay
the rejected command. Other direct workflows, including Send to printer storage,
reprinting a file already on the printer, and calibration, still use the original network plugin and may remain restricted
by firmware.

## Integration contract

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

On macOS, the launcher addresses `com.bambulab.bambu-connect` using `/usr/bin/open`
with separate arguments. Other platforms use the OS URL handler. No credentials,
signing keys, private Connect IPC, or GUI automation are used by the integration.

Exports use the existing sliced-3MF exporter, including this fork's thumbnail
view-angle customization. Each handoff gets a unique `.gcode.3mf` in the Studio
data directory's `bambu-connect` subdirectory, independent of live slicing files.
Snapshots survive Studio closing and are removed on a later handoff once seven
days old. Reopen a week-old job from Studio instead of relying on Connect's old
import. A failed export never launches Connect; a failed launch offers install
and sign-in guidance. Manual sliced-file export remains available.

## Validation

Verified on September 13, 2026:

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

For future regression checks, repeat the above and check a selected non-first
plate, missing sliced data, and the rejected-control dialog. The last
dialog's layout and recovery action should be checked when the network plugin
returns `unsigned_studio`.

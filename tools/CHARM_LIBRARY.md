# Charm & Pair library console

Run `./charm-library` from the repository to open the **royal32/omnitrix** terminal interface. Use arrows and Enter to browse, edit the options, then choose **Run command** or press F5. Tab switches to columns, `/` searches, and q quits. `./charm-library --plain` uses numbered menus. Every operation is also a direct CLI command.

The existing collection has been adopted into persistent state: **261 projects, 698 managed sliced plates, and 654 vector previews**. The one older, unmapped Wanda export remains untouched. Forty-four tracked plates retain their original thumbnails because of painting, filament mismatches, or reviewed non-flat geometry. **Library → Exceptions** explains each fallback.

## Everyday use

1. Save new editable `.3mf` projects into `.charm-library/inbox/`, or use **Sources → Add** to register a file or a folder. Choose **Pair Eyewear** for frame projects; the inbox defaults to **Bogg Charm**. Folder discovery is recursive.
2. Run **Library → Plan**. It reads and hashes the inputs and outputs without changing them.
3. Run **Library → Update**. Leave the project filter blank for everything, or enter part of a name/ID. Limit `0` means all projects; use `10` for a small batch.
4. Refresh the sliced library in Studio. Output defaults to `~/Desktop/Sliced_Temp`.

```sh
./charm-library sources add --path "/path/to/Pair projects" --category "Pair Eyewear"
./charm-library library plan
./charm-library library update --limit 10
./charm-library library previews --project Bee --force
./charm-library maintenance verify
```

An update converts projects to the established **A1 mini 0.4 nozzle – 1.1.0**, **Atomic PLA  A1 Mini 1.2**, and **0.20mm Charms @BBL A1M 1.0.3** presets with a Smooth/High Temp plate. X1 source projects are eligible for conversion. Model dimensions and orientation are preserved; layouts can be translated, packed, or split to fit. Models that cannot fit are reported for review.

Plate names use existing meaningful names and model parts, with **SB** for a screw back and knob and **Hanging Hole** where applicable. Name collisions receive `Design N` while preserving SB. Use **Library → Projects** to find IDs, plate indices, source paths and saved editable project paths; **Library → Rename** sets an explicit name for the next update.

The importer preserved editable copies of the old prepared inputs under `.charm-library/imported-sources/`. It does not continue watching their old iCloud/NAS/PC originals. To replace one with a newer version, use **Sources → Replace**:

```sh
./charm-library library projects --project Bee
./charm-library sources replace --project Bee --path "/path/to/new/Bee.3mf"
./charm-library library plan --project Bee
./charm-library library update --project Bee
```

Saving changes to an already tracked source path also triggers an update. Different filenames with different contents are treated as separate projects; the script does not guess version relationships from dates or filename suffixes. Exact duplicate source contents are skipped. Raw STL and model-only 3MF files need to be saved as configured slicer projects before processing.

## Incremental behavior and recovery

- A second run with unchanged inputs does no slicing and leaves print files, previews and the manifest unchanged. An operation log is refreshed.
- Source content, resolved profile contents, slicer executable, preparation/export code, plate-name overrides and destination changes invalidate the slice. Content hashes are used instead of timestamps.
- A vector-generator change or missing sidecar regenerates artwork without slicing. **Previews** refuses projects that first require a slice update; `--force` explicitly retries otherwise unchanged previews.
- Every managed output is hash-checked. Edited files block that project. Untracked filenames are preserved. Missing managed files can be rebuilt. Shared outputs are protected against differing replacements.
- Each project is staged and validated before publication. Replaced/retired managed files are backed up under `.charm-library/archive/`. A publication journal recovers interrupted copies on the next mutating operation. An operation lock prevents competing runs. Cancel with Ctrl-C or F9; completed projects remain usable.
- Missing source folders/files never cause print files to be deleted. Failures retain staged inputs, slicer commands and `slice.log` under `.charm-library/work/`. Successful runs also retain a full editable `prepared.3mf` there.

Vector sidecars preserve the original printer-compatible PNG thumbnails and G-code. Negative-volume holes are included in supported flat previews. Existing reviewed PNG fallbacks are retained when rebuilding. Unsupported painting, inconsistent filament assignments, or a plate taller than 20 mm produce a reported PNG fallback. Review new previews for geometry that is not well represented by a flat front-face section.

Keep `.charm-library/` backed up: it contains the source snapshots, configuration, manifest, saved projects and backups. These local files are ignored by Git. Never delete the manifest to “start fresh” against an existing collection: it owns the integrity and replacement records.

## Setup and configuration

Configure the processing machine before the first update. **Settings → Show/Configure** controls source/output locations, the slicer executable, the profile root and exact preset names. Blank configuration fields retain their current value. **Maintenance → Doctor** resolves preset inheritance and checks the executable and Python dependencies.

On another Mac, install Rust/Cargo and Python 3.11 or newer, authenticate Git for the private Omnitrix repository, then fetch its pinned dependency once:

```sh
cargo fetch --locked --manifest-path tools/library-console/Cargo.toml
./charm-library maintenance setup
./charm-library maintenance doctor
```

Normal launches build offline from the pinned Cargo lockfile. Setup installs NumPy and Shapely into an isolated `.charm-library/venv`; it does not alter system Python. `CHARM_LIBRARY_HOME` selects separate configuration/state, and `CHARM_LIBRARY_PYTHON` overrides the interpreter. The backend can also run as `python3 tools/library_backend.py --help` without building the terminal interface.

**Maintenance → Adopt** is the one-time migration for the old batch reports. It copies and verifies permanent source snapshots and verifies existing print jobs without modifying them. Normal operation has no dependency on the historical scripts, temporary folders, or report directory.

## Validation

```sh
PYTHONDONTWRITEBYTECODE=1 .charm-library/venv/bin/python3 -m unittest discover -s tests -p 'test_charm_library*.py'
PYTHONDONTWRITEBYTECODE=1 .charm-library/venv/bin/python3 -m unittest discover -s tests -p test_vector_preview_generator.py
CHARM_LIBRARY_INTEGRATION=1 .charm-library/venv/bin/python3 -m unittest discover -s tests -p test_charm_library_slicer.py
```

The opt-in integration test uses the local adopted Bee, Grinch and Wanda projects, slices nine plates into a temporary library, validates G-code/3MF/profile/preview integrity, then checks that a second run leaves the outputs untouched. Studio's thumbnail renderer requires normal macOS graphics access. It may abort in an automation sandbox even when G-code slicing succeeds.

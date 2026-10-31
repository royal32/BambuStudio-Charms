# Local cleanup shortlist

**Completed 2026-10-31:** the approved ZIPs, older app copies, generated tests
and compiler outputs were removed (48 paths). Compiled dependencies were
retained. The tables and path lists below record the pre-cleanup inventory.

Measured after installing the library/shortcut/layout fixes on both Macs and
removing this investigation's eight temporary diagnostic app copies. The working
application is `/Applications/BambuStudio.app`. The library processor is also
configured to use that installed executable, so it does not depend on the build
tree's executable.

Sizes below are `du` totals, not guaranteed reclaimed space: APFS clones share
blocks between app copies. Rebuild times are rough planning estimates, not timed
benchmarks. The checkout's `build/` currently totals about 20.7 GiB.

| Candidate | Reported size | Cost after removal |
| --- | ---: | --- |
| Old release ZIPs and test ZIPs under `build/` | 1.24 GiB | No rebuild needed for installed apps. Repackage the current release in about 1–3 minutes if needed. |
| Older review/release app copies and older installation backups listed below | 8.77 GiB | No rebuild needed; the current installed apps and current release bundles remain. Old versions would require checking out their code and rebuilding if ever needed again. |
| Generated test executable/output directories under `build/arm64/*checks*` and `*regression*` | 2.84 GiB | Usually about 1–10 minutes per test target to compile/run again with dependencies retained. |
| `build/arm64/src` and `build/x86_64/src` (compiler objects, binaries and symbols) | 5.27 GiB | Full Studio rebuild; allow roughly 45–120 minutes per architecture. Installed apps and the configured library processor keep working. |
| `deps/build` | 2.05 GiB | Rebuild/download dependencies before the next source build; allow roughly 1–3 hours per architecture, potentially longer with network or memory pressure. |

The lowest-effort first pass is old ZIPs and older app copies. The current
release bundles and one local rollback app are excluded from that app-copy
total and should stay until the updates have had normal use.

## Older app copies

All paths are relative to `<repository>/`:

```text
build/arm64/Installed-Backup-20261028
build/arm64/Installed-Backup-20261029
build/arm64/Installed-Backup-20261030
build/arm64/Installed-Backup-Apple-Signing-20261030
build/arm64/Installed-Backup-Keychain-Window-20261030
build/arm64/Installed-Backup-Chat-Runtime-20261030
build/arm64/Installed-Backup-Terra-High-20261030
build/arm64/Assistant-Capabilities-Review
build/arm64/Assistant-Geometry-Review
build/arm64/Assistant-Workbench-Review
build/arm64/Terra-High-Release
build/arm64/Chat-Runtime-Release
build/x86_64/Assistant-Geometry-Review
build/x86_64/Assistant-Workbench-Review
build/x86_64/Terra-High-Release
build/x86_64/Chat-Runtime-Release
```

The largest release archives are:

```text
build/assistant-chat-runtime-intel.zip
build/Charms-Assistant-Workbench-Intel-20261030.zip
build/Charms-Intel-Install-20261028.zip
build/Charms-Assistant-Geometry-Intel-20261029.zip
build/Charms-Terra-High-Intel-20261030.zip
```

## Keep

- `/Applications/BambuStudio.app` on both Macs.
- `build/arm64/Library-Shortcuts-Layout-Release` and
  `build/x86_64/Library-Shortcuts-Layout-Release`: complete, signed current bundles.
- `build/arm64/Installed-Backup-Library-Shortcuts-Layout-20261030`: the pre-update
  local app. The Intel rollback is in the Downloads/Charms-Backups folder.
- Repository source, `.git`, and `.charm-library` state/configuration.
- `.charm-library/work` (about 1.00 GiB) and
  `.charm-library/imported-sources` (about 0.34 GiB): the processing state still
  references these inputs; they need a separate source-reference migration
  before cleanup.
- The actual shared library's `Projects/`, sliced `.gcode.3mf` files and shared
  `.charm-library` records: these are the working collection, exclusions and
  feedback, rather than build artifacts.

The cleanup recovered about 10.5 GiB of available disk space, leaving about
25.3 GiB free immediately afterward. The retained installed app, current release
bundles and rollback app have unchanged executable/resource-manifest hashes and
passed deep code-signature verification. The compiled dependency tree's file
metadata fingerprint remained unchanged. Library inputs and shared data were
outside the removal paths.

The exact removal report is in `build/approved-cleanup-2026-10-31.json`.

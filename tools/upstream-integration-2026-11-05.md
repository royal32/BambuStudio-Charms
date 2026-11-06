# Upstream integration — November 5, 2026

The integration branch is `codex/upstream-20261105`. The verified pre-merge
snapshot remains on `origin/codex/windows-bambu-connect` at `c6aa14538`.
The fork's default branch and installed applications are unchanged.

## Publication review

Before merging, the existing fork rebuilt successfully and passed native
startup, model-import/material-mapping, support-generation and geometry checks.
The publication review covered 58 unpublished commits and 386 file versions.
Personal environment references were removed from unpublished file history and
commit messages. Normal author attribution was preserved. The credential scan's
only matches were the explicitly synthetic Keychain test constants.
Account data, conversation logs, local model archives and compiler outputs were
not published. Original local history is recoverable from a verified private Git
bundle under `.git`, rather than ordinary branches that could be pushed by mistake.
The final merged fork history was scanned separately from public upstream
changes, with no additional private environment references or real credentials.

Standalone assistant checks now link their actual shared tool definitions
without needing a full slicer build. Profile-definition assertions remain in
the linked native model suite. This changes test wiring, not assistant policies.

## Integration choices

Upstream is `355d71eee`, version `02.09.01.70`: 1,352 commits since the fork's
previous upstream merge. It includes a CLI backup-thread exit fix in addition
to slicing, support, filament, printer, menu and logging changes.

- **Plane cutting:** retain upstream's shared cap-welding fix (`0394368c8`) and
  the assistant's near-plane snapping correction. Testing without the latter
  still produced an open edge for one of two equivalent planes. With it, both
  origins, cap closure, physical dimensions and transformed cuts pass. These
  fixes address distinct numerical cases.
- **Mesh Boolean:** the assistant uses upstream's backend-neutral dispatcher,
  following the application's OpenMeshCraft/MCUT selection rather than forcing
  MCUT independently. Existing cancellation, paint retention and result checks
  remain in place.
- **Paint preservation:** adopt upstream's extensions for native Boolean, merge
  and simplification (`d19070a05`). Assistant cuts and remeshing continue through
  the shared native paint-reprojection APIs; no second paint engine is added.
- **Menus:** use upstream's popup lifetime/hover fixes and collapsed-account-menu
  placement, retaining the fork's account-switch actions. The Objects-search
  reentrant-popup guard remains necessary; upstream does not contain it.
- **Filament controls:** use upstream's relocated AMS/settings buttons once,
  retaining the separate apply-first-preset button. Preserve immediate digit
  assignment and the macOS Objects-list keyboard routing. Upstream's CJK
  character-shortcut fix (`204e1e885`) covers a different input path.
- **Project imports:** use upstream's explicit global-feature filament context
  and retain source-global feature colors as remapped object overrides.
- **Printing:** retain Connect handoff, interactive previews and deferred
  warning-dialog dismissal. Apply upstream's send-version policy centrally to
  Connect handoffs, and keep native bare-G-code handling and Linux printing.
- **Plug-in validation:** upstream's certificate-rotation support remains;
  the fork's Apple-anchored official-publisher check is still needed because
  private Studio builds have a different signer from Bambu's plug-in.
- **Test setup:** use upstream's shared `test_gui` interface instead of the
  fork's duplicate wxWidgets include/link setup.

Upstream also fixes recent-project thumbnail loading stalls (`52e1df3c4`) and
CLI exit after a backup-thread failure (`355d71eee`). Those are retained, while
the library's worker/transaction fixes remain: they cover different operations.

The assistant, embedded vector previews, source-project actions, shared-library
publication/exclusions/feedback and Mac-only processing console remain distinct
fork features. Upstream does not replace those workflows.

## Dependencies and validation

Existing compiled dependency bundles are retained. Only the updated Assimp
importer is rebuilt in separate prefixes, including upstream's FBX/glTF fixes.
Windows dependency sub-builds receive the parent's Visual Studio SDK overrides.

Completed:

- Full ARM and Intel application builds, stable code signing, self-contained
  bundle audits and native CLI startup. The Intel package passes the macOS 12.6
  audit; the ARM review package requires macOS 26 because of the retained local
  Imath/zstd libraries and was checked on macOS 27.2.
- Full Windows x64 application/wrapper build, isolated installation and native
  CLI startup. All nine standalone checks pass: five assistant, two library,
  and both shared-runtime/native-Windows Connect suites. Windows GUI and live
  account handoff were not exercised.
- Intel geometry, equivalent-plane, mesh Boolean, vector packaging, real-model
  import/remapping and support-generation tests executed on macOS 12.6.
- ARM geometry, equivalent-plane, Boolean, surfaces/supports, transforms, text,
  vector packaging and scripted complete-workflow tests. The five source
  projects and disconnected-shell STL fixture pass import/material checks.
- Merged web frontend build/legacy-CSS check, device-page host contract,
  18 homepage/Connect/library JavaScript tests, five assistant CTest suites,
  two library CTest suites, Connect runtime CTest, and 50 Python tests (two
  opt-in integration skips in the normal discovery run).
- Both opt-in tests then passed against the review slicer with temporary
  outputs: nine plates exported, unchanged refresh skipped rebuilding, and a
  saved editable-project change rebuilt without overwriting the edited source.
- Synthetic native chat checks: light/dark and narrow/wide layout, Unicode,
  selection retention, font changes, streaming and long-token sizing.
- Account-free local GUI smoke checks: library/vector display, source chooser
  cancellation and subsequent context-menu recovery, source opening, immediate
  slot assignment ending at slot 4, assistant layout, and the tall print dialog.

Two MSVC portability problems found while building the current fork UI were
corrected: a nested-lambda default argument and collision with the Windows
`small` macro. Standalone tests now declare their Boost locale dependency,
compile UTF-8 fixtures correctly and close their feedback file before Windows
cleanup. The production library path handling was already using UTF-8 paths;
the test now constructs its expected paths in the same portable form.

Installed applications, account profiles and the real library are unchanged.
No authenticated GUI was launched over SSH, no real assistant model requests
were made, and no print job was submitted. Authenticated Connect handoff and
physical keyboard/device acceptance were not repeated in this integration run.

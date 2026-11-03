# Responsive library publication — November 2, 2026

Adding a plate previously read every shared project registration on the GUI
thread, before creating its progress dialog. The live hang sample showed
`plan_publication` blocked in `pread`; individual tiny iCloud records could take
roughly a second to read.

Publication now shows progress first and uses an independent file worker for
shared-folder checks, hashing, staging, replacement and cleanup. Registrations
are read once with at most eight concurrent reads and reused for source and
ownership lookup. The GUI exports a local model/slice snapshot only after the
worker captures the existing-file baselines. Excluded plate slices are not exported.
The GUI checks slice validity before approving replacement. Existing aliases,
source identities, collision checks, concurrent-change checks and rollback
behavior remain intact; no shared metadata migration is needed.

Cancel and a two-minute pre-replacement wait limit return without joining a
blocked OS read. Workers own immutable input and scratch lifetimes; cancelled
work cannot publish later. Only one file operation can remain outstanding.
Once replacement begins, cancellation is disabled until success or rollback.

Validation completed:

- Standalone native storage/worker CTest checks pass, including an intentionally
  blocked read, prompt cancellation/destruction, prevention of late publication,
  commit approval, error forwarding, eligible snapshot plates and ownership.
- Native Intel worker/storage checks pass on an Intel Mac running macOS 12.6.
- Python library tests: 37 run, 2 platform skips; library web tests: 6 pass.
- Both full release builds and final incremental builds pass. The signed Intel
  bundle passes architecture, dependency and macOS 12.6 deployment auditing.
- Live ARM review used a disposable portable profile/library. Namaste was
  sliced and published through the actual menu; archive CRCs and saved source
  and output SHA-256 values matched independently. During a deliberately slow
  existing-file hash, the progress window remained accessible and Cancel
  returned to the main window. Registration and source hashes stayed unchanged.
  The sparse test file was restored. No print was sent or real library changed.
- Both installed bundles pass signature/hash and native CLI startup checks.
  No authenticated GUI or Connect session was launched over SSH.

Installed at `/Applications/BambuStudio.app` on each Mac, signed with the
existing Apple identity. Previous apps are retained as rollback copies.

Executable SHA-256:

- ARM: `60d298b7d04adc24ad35503dc28fe5c64e1da2c1c64831c8c49b37a252e9ed23`
- Intel: `d93239108241f9a1fd740890b40a836c7924bd4baf0a94b7364ec0262a52e3b1`

Evidence logs and isolated fixtures are under `build/library-worker-*`.

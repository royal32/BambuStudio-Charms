# Assistant chat containment and environment awareness — October 30, 2026

Both `/Applications/BambuStudio.app` copies (the ARM Mac and the Intel Mac
running macOS 12.6) are updated. Their previous bundles are preserved. Both use
the existing Apple Development signing identity and app identifier.

## Changes

Message cards and the transcript now explicitly clip native Cocoa children
and compositor layers. The transparent-background helper handles both a native
text view and its scroll-view wrapper. The dialog lays out again after showing,
so its initial text layout uses the displayed window's geometry.

Each assistant request receives an application-generated runtime contract.
Its tool names come from the live registry, and its turn budgets share the
execution loop's constants. It explains project-state restrictions, local-file
and library access, model views, physical material uncertainty, human-only
editors and confirmations, and final print submission. The live context includes
the current confirm-every-edit preference. The contract is also recorded in
the local conversation log.

Instructions require the assistant to distinguish a temporary state or tool
limit from a missing capability, combine supported operations, and avoid
claiming arbitrary desktop/browser control or completion of an editor it has
only opened.

## Validation

- Both release builds completed. The Intel bundle passed the architecture,
  dependency, deployment-target and signature audit for macOS 12.6.
- Client/session contracts passed on both the ARM Mac and an Intel Mac.
  They verify that the advertised registry matches the actual tools and the
  runtime contract reaches the model request.
- The synthetic native chat fixture passed light/dark, Unicode, streaming,
  selection-preservation, font-change, wrapping, resizing and scrolling checks.
  Scrolling covers widths 400/520/700, heights 340/480 and four scroll positions.
  A saved pre-fix native implementation fails the explicit compositor-clipping
  invariant; the fixed implementation passes. This is a containment regression,
  not a claim that the original screenshot was exactly reproduced locally.
- Local screenshots show light message text staying below the selector and
  above the footer while scrolled. The installed assistant's first message and
  model selector also render correctly and reconnect without a Keychain prompt.
- A real Terra High read-only request correctly described loop trimming and
  a 2 mm hole, noticed the empty project, and identified MakerWorld downloading
  and final printer/material selection/submission as human steps. It made no
  project changes.
- Transfer and installed executable hashes match. The private credential-helper
  transport check passed on the Intel Mac without opening an account or GUI.

The Intel test machine's installed app remains closed for a normal desktop launch. Its final
light-mode UI check on macOS 12.6 is still a human check; no authenticated Studio
or Connect GUI was launched over SSH.

## Installed artifacts

| Machine | Executable SHA-256 |
| --- | --- |
| ARM | `c7e3a52d67ae584f91eab8ecd4488f0f925981e93d798d1f8ff113711cc9c13c` |
| Intel | `9245412c73a8aab81d5173a53fb9d5107195e9743637ee048e2b33bee67cba3e` |

The ARM backup is at
`build/arm64/Installed-Backup-Chat-Runtime-20261030/BambuStudio.app`.
The Intel backup is at
`~/Downloads/Charms-Backups/Before-Chat-Runtime-20261030/BambuStudio.app`.
Build, signing, contract, audit and installation logs use the
`build/assistant-chat-runtime-*` prefix. The standalone native fixtures are
disposable; their test processes were stopped after verification.
No print job was sent. No git push was performed.

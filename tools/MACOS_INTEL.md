# Intel Mac distribution

The 2017 Intel Mac uses `x86_64`; the normal Apple Silicon build cannot run on it.
Build and package a separate Intel app, targeting macOS 12.0 or earlier. The
processing console stays on the Apple Silicon Mac; it is not part of this distribution.

Use the Intel Bambu dependency bundle, with the repository's wxWidgets patch
applied. Do not use Apple Silicon Homebrew libraries. Set these CMake options
when configuring a separate `build/x86_64` directory:

```sh
-DCMAKE_OSX_ARCHITECTURES=x86_64
-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
-DCMAKE_MACOSX_BUNDLE=ON
-DBBL_RELEASE_TO_PUBLIC=1
-DBBL_INTERNAL_TESTING=0
-DCMAKE_PREFIX_PATH=/absolute/path/to/Intel/dependencies/usr/local
-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew
```

Set `CMAKE_OSX_SYSROOT` to an installed SDK. The compatibility build uses
MacOSX26.5.sdk; the deployment target controls the oldest supported OS.
`PNPM_STORE_DIR` can point to the existing ARM build's store when the two builds
share the frontend's `node_modules` directory. This avoids pnpm prompting to
replace that directory during a second architecture's build.

The dependency bundle's `wx-config` and imported OpenCV/Assimp targets can
contain paths from CI. Relocate those paths before configuring. If the SDK
lacks liblzma headers, architecture-independent xz headers can be used with the
SDK's `usr/lib/liblzma.tbd`; do not link Homebrew's dylib.

Copy the completed app to a distribution directory and replace its Resources
symlink with a real copy of the resources. Remove build-machine runtime search
paths and bundle any required non-system dylibs. Audit the actual distribution:

```sh
python3 tools/audit_macos_bundle.py /path/to/BambuStudio.app --arch x86_64 --macos 12.6
codesign --verify --deep --strict /path/to/BambuStudio.app
```

The audit checks every bundled Mach-O file, minimum OS versions, architectures,
runtime search paths, external symlinks and non-system dependencies. Passing it
does not replace a native launch check. An ad-hoc signature is not Developer ID
signing or notarization.

On Intel, sign in to the same Bambu account and region in Studio and Bambu
Connect 2.5.0-beta.15. Intel deliberately does not use the private Apple Silicon
network-plugin session reader. Connect must finish its own first-run setup.

For review, use `--datadir` with a separate profile. Copy only the official
compatible plugin binaries into that profile, never another person's settings,
login data or account registry. Test library previews, right-click actions,
the print dialog and AMS mapping, then stop at Connect's review screen without
sending a print.

## Compatibility validation, 2026-10-12

The separate Release build targets macOS 12.0 and uses Intel Bambu dependencies
with the patched wxWidgets libraries. Its packaged executable links only to
macOS system libraries/frameworks; the distribution audit and ad-hoc signature
verification pass. The app's resources are copied into the bundle.

Native checks on an iMac18,1 running macOS 12.6 pass for:

- Full Studio executable startup, `--help`, and loading the named Band Mom A1
  Mini project with `--info`.
- Vector geometry, occlusion, holes and marching-ants boundaries.
- Shared library storage, source links, deletion and feedback records.
- Embedded preview identity/fingerprints, Unicode filenames and PNG fallbacks.
- Intel's explicit exclusion of the private ARM session reader.
- Official Intel Connect signature verification and bridge transport, version,
  renderer and internal store bindings.

The Apple Silicon session-reader regression tests and library/Connect JavaScript
tests also pass. The app is staged with a separate review profile and five real
library samples. Visual print-dialog and authenticated Connect import checks
remain a separate acceptance check; transport success alone does not prove them.

Local package: `build/x86_64/BambuStudio-Charms-Intel-macOS12.zip`.
The review launcher on the Intel Mac is
`~/Downloads/Charms-Intel-Review/Launch Charms Review.command`.

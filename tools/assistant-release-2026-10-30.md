# Terra High and stable plane cuts — October 30, 2026

Both Applications copies of BambuStudio.app are updated, with their previous
bundles preserved as backups. The same existing Apple Development identity
signs both architectures; no trust exceptions or credential exports are used.

## Behavior

- Terra High is adopted once as the default when the connected account offers
  it. Subsequent manual model choices persist. Its label, request bodies and
  conversation logs explicitly say High; Luna remains explicitly Max.
- Vertices within 10 nm of a cut plane are snapped to it before capping. This
  removes the classification disagreement that made equivalent plane origins
  behave differently. Cuts introducing additional open edges are rejected before
  model mutation.

## Validation

ARM and Intel builds complete. Native geometry checks pass locally and on
an x86_64 macOS 12.6 Intel Mac: both reproduced plane origins, rescaled
normals, both retained sides, closed caps, expected volumes, unchanged poses,
paint preservation, through-hole sections and actual sliced holes. The four
scripted native workflows and client/session contracts pass. The latter also
verify Terra's High request and logged effort.

The Intel bundle passes the architecture/dependency/deployment/signature audit.
Transfer and installed executable hashes match the signed local package. Its
private credential-helper transport guard also passes at the final installed
path without reading an account or launching the GUI.

On the Apple Silicon Mac, the updated assistant connects without a new Keychain prompt,
selects GPT-6.1 Terra (High), and completes a read-only request in the actual UI.
The Intel test machine's app is left closed for a normal desktop launch; no authenticated Studio
or Connect GUI was launched over SSH. The older signing identity may require
the explicit one-time assistant Keychain approval on that first launch.

## Installed artifacts

| Machine | Installed path | Executable SHA-256 |
| --- | --- | --- |
| ARM | `/Applications/BambuStudio.app` | `8c2c335b028aa691aa767b00d5a831c5bea6bfb3cbcc73a571f98d897be9bad9` |
| Intel | `/Applications/BambuStudio.app` | `76dd9a4c63c3ce4b3660b8b5c317219f8104ed0ed01d10909f1e594eaf58a53f` |

The ARM backup is under
`build/arm64/Installed-Backup-Terra-High-20261030/BambuStudio.app`.
The Intel backup is at
`~/Downloads/Charms-Backups/Before-Terra-High-20261030/BambuStudio.app`.
Detailed build/install/audit logs are the `build/assistant-terra-*` artifacts.
No print job was sent and no git push was performed.

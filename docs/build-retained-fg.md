# Build the release's retained frame-generation module

Use the corresponding-source archive for **2026.11.15.1** when rebuilding its retained host and module.
The public branch can advance independently. A build from the branch tip is not automatically the source
cohort of the supplied binary.

The selected release artifacts are:

| Artifact | SHA-256 |
| --- | --- |
| `virtualdesktop-openxr.dll` | `f1f38d3f01be1db4f5d99b60f1cabf7f6094a238e859b45ff6a14463b4ce0bc5` |
| `vdxr-frame-generation.dll` | `7a035e5c14ac2b4a5928441ffa834324a719d17fe56b83a2b4b8e3b04eed0fa5` |

The archive's `SOURCE-MANIFEST.json` records the retained inputs and any path relocations. Follow its
`BUILD-SOURCE.md` and retained-host staging instructions for the host, and the module recipe identified by
the module's own source/build manifest. Keep the host and module checkpoint headers with their respective
builds; do not substitute an older release's prepared synthesis files, generated shader bytes or CMake cache.

Supply the external SDK/compiler/dependency inputs identified by those recipes separately. The release does
not collect installed NR DLLs. Build receipts must record the actual tools, flags, source inputs and output
artifacts. Packaging a source directory does not establish an equivalent build or matching runtime behavior.

The older module hash `8206b8a8…` and its build flags describe the previous release, not this module.
Use the current archive's pinned recipe rather than carrying those flags forward. A rebuild is not promised
to reproduce the selected binary byte-for-byte; compare the resulting receipts before using it.

For installation, use the [quickstart](NR-FG-QUICKSTART.md) and [side-by-side guide](SIDE-BY-SIDE-RUNTIME.md).
The host's MIT license and the module's LGPL-3.0 and third-party notices remain separate; see the
[module notice](../nr-frame-generation/NOTICE.md).

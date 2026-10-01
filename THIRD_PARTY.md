# Third-party inventory

| Component | Source / license evidence | Distribution in this snapshot |
| --- | --- | --- |
| Nexus-authored code | Root LICENSE and per-package Makefiles; Nexus Community License 1.0 | Source |
| OpenWrt Node recipe | `feed/nexus-node-runtime/Makefile` and adjacent `COPYING`; GPL v2 notice retained | Recipe and patches, not Node binaries |
| Node.js | Version, archive checksum and upstream LICENSE named in the Node recipe; MIT plus bundled dependency notices | Downloaded by build; retain upstream notices with binaries |
| Argon | `dependencies/luci-theme-argon.lock` and `.NOTICE.md`; Apache-2.0 | Checksum-locked external source, not vendored |
| OpenWrt / LuCI and runtime libraries | Dependencies declared in each `feed/*/Makefile`; upstream package licenses | Supplied by target SDK / firmware distribution |

The root Apache license does not replace upstream terms. For binary releases,
record the actual SDK/feed revisions, complete package manifest, licenses and
corresponding source archives required by those licenses, including local patches
and build configuration. This source inventory alone is not a firmware SBOM.

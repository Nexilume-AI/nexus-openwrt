# Release checklist

## Source candidate

- [ ] Review the exact source snapshot and its SHA-256 manifest, ownership and third-party notices.
- [ ] Run Debug host tests, all Relay tests and desktop bundle tests in a fresh checkout.
- [ ] Inspect the candidate for secrets, private URLs, personal data and stale documentation.
- [ ] Create a separate Git repository with fresh history; do not push the mixed product repository or tags.
- [ ] Set repository description, license display and private vulnerability reporting on GitHub.
- [ ] Require CI and review on the default branch; keep Actions permissions read-only by default.
- [ ] Choose release version/tag and visibility after reviewing the candidate; record CI results for that exact commit.

## Packages / desktop images (separate gate)

- [ ] Build using matching official SDK/ImageBuilder and retain checksums, revisions, configurations and source/license materials.
- [ ] Verify package signatures and dependency resolution on a clean target; never distribute signing private keys.
- [ ] Test installation, upgrade, reboot, LuCI, registration/invocation and the advertised Direct/Relay modes.
- [ ] Build an unbooted desktop image and test a disposable copy, including local IPv6, upstream IPv6, isolation and repeat startup.
- [ ] Verify no credentials, identity state, real addresses or logs were baked into the release image.
- [ ] Publish binary checksums, package list, supported target matrix and limitations with the release.

Do not mark these items complete from host unit tests alone. No VM image is
approved by the existence of the launcher or packaging helper.

## Signed package feed

Use a dedicated OpenWrt 25.12.4 x86/64 SDK and a clean Linux checkout. Build with
`JOBS=2 sh scripts/build-package-release-sdk.sh /path/to/sdk`. This replaces the
SDK configuration, selects all three profiles, and can take substantial time
to compile Node.js. Retain the resulting `.config` and official feed commits.

Commit the release source, then run `python3 scripts/package-release.py --help`.
The packager takes the SDK, its checksum-verified archive, build configuration,
a protected signing private key, its public key, a version, and a **new** output
directory. It selects exact package versions from the recipes, verifies matching
keys, signs the APK index, and includes licenses, provenance and SHA-256 sums.
It never copies the SDK or its signing directory wholesale. Keep the private key
outside release artifacts, with mode `0600` in a `0700` directory; back it up
through the operator's protected key-management process.

Only publish the explicitly reviewed archive, public key and outer SHA256SUMS.
Publish the public-key fingerprint in release notes. Run installation, upgrade,
reboot and invocation tests before publishing; the packager is not an acceptance
test. Never upload a test VM, SSH private key, live configuration or raw lab logs.

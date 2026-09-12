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

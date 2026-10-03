# Contributing

Issues and pull requests in English or Chinese are welcome. Bug reports should
include the target device model, OpenWrt and package versions, reproduction steps,
and expected and actual behavior. Remove device credentials and personal data
from logs. For security vulnerabilities, follow [SECURITY.md](SECURITY.md).

1. Make changes on a new branch. Describe the user-visible changes and affected
   deployment modes.
2. Run the Debug CMake, Relay, and desktop packaging tests listed in the README.
   Changes to LuCI, UCI, or package installation behavior also require validation
   on a matching OpenWrt device. State which environments you have not tested.
3. Update the relevant English and Chinese user documentation. Bump the
   corresponding `PKG_RELEASE` when package behavior changes.
4. Preserve third-party copyright and license notices. Document the source,
   version, and checksum of new dependencies. Only submit code you have the right
   to contribute under the applicable file license. Do not include Cloud data,
   real certificate private keys, device backups, disk images, or build logs.

Maintainers review correctness, interface compatibility, and test evidence in
proportion to the scope of the change. There is currently no guaranteed response
time or long-term version support commitment. Host-based CI tests do not replace
hardware or VM acceptance testing.

## Contribution licensing

Nexus-authored changes are distributed under the Apache License 2.0 (modified).
Read [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and the
[Nexus Contributor License Agreement](CONTRIBUTOR_LICENSE_AGREEMENT.md).
Every contributing author must explicitly accept that agreement for their PR
before merge; maintainers must record the acceptance as described there.
Contributors retain copyright while permitting commercial use, dual licensing
and future relicensing. Historical contributions and third-party code are not
automatically subject to the new grant. Preserve all upstream notices.

Licensing inquiries: cary.nexilume@outlook.com. Every contributor must explicitly
accept the contributor agreement for the PR. Checking a template box or a
maintainer's declaration does not constitute another author's consent.

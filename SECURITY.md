# Security reporting

Do not disclose exploitable vulnerabilities or credentials in a public issue.
Once this repository is hosted on GitHub, use **Security → Report a vulnerability**
when private vulnerability reporting has been enabled by the repository owner.
If that entry is unavailable, request a private reporting channel from the
maintainer without including exploit details. No private contact address is
claimed by this source snapshot.

Include affected source/package versions, deployment mode, reproduction steps,
impact and a minimal redacted example. Never send live enrollment codes, TLS
private keys, Relay signing keys or device backups. Acknowledgement and fix
timelines are not yet guaranteed; support currently targets the current source
candidate, with no promised backport window.

Before deployment, replace all example credentials, retain TLS verification,
limit management access and review which ingress modes you enable. LAN No-JWT
interfaces must not be exposed as authenticated public endpoints. Open Mesh
discovery, Cloud-managed trust and self-hosted seed roles have different trust
boundaries; consult the corresponding user guide.

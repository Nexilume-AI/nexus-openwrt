---
sidebar_position: 8
title: Peer Trust
---

# Peer Trust

This page performs stateful trust operations that affect routes.

| Section/field | Purpose |
| --- | --- |
| Pending trust | LAN/SVCB candidate, endpoint, evidence, and lease |
| Review & trust → Peer ID | Stable local ID for the dynamic Peer |
| Review & trust → Graceful restart | 5–300 seconds of controlled stale-route grace |
| Trusted dynamic peers | Admission source, lease, and revoke action |
| Authorized Agent Cards | Issuer, key, capabilities, revision, and revoke action |
| Directory Card trust keys | Signed bundle Router/key/status/SHA-256 evidence |

`agentd` validates candidate generation during approval; a changed candidate fails without altering the live table. Revocation removes learned capabilities. Auto-admitted candidates can return, so also change admission mode or its allowlist. DNSSEC without an authorized Card proves naming data, not business authorization.

Self-hosted OpenWrt Open Mesh and Cloud Relay have separate connection state and trust configuration. See [Cloud Relay](../../guides/cloud-relay.md) for Cloud (including Community), or [node roles](../../guides/router-roles.md) for self-hosted seeds.

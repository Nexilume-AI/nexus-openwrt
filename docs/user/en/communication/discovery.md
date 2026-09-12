---
sidebar_position: 3
title: Discovery, trust, and paths
---

# Discovery, trust, and paths

Discovery answers “who might be reachable,” admission answers “may it become a Peer,” and route propagation answers “what capabilities does it offer.” Observe these stages separately.

## LAN DNS-SD

Routers publish and consume `_agent-router._tcp.local` records. Candidates contain bounded metadata such as Router ID, domain, endpoint, and lease time—not prompts, tokens, or Agent payloads.

Current Open Mesh defaults automatically admit validated LAN Routers. For manual, same-domain or allowlist control, first select **Managed peer trust** in **Developer mode → Advanced Settings → Router Mesh**, then configure LAN admission. Matching domain names do not establish a default isolation boundary.

## DNSSEC SVCB

Cross-domain discovery queries `_agents.<domain>` SVCB records. The Router accepts results only when a local validating resolver provides authenticated DNSSEC data. The target, port, and `ipv4hint` remain candidate metadata; an address hint is not trust evidence.

SVCB finds a Router across administrative domains; it is not an Agent transport. Open Mesh can automatically admit DNSSEC-validated candidates; Managed peer trust uses Peer Trust and Agent Card / Directory policy. Both modes still require capability announcements before routes become selectable.

## Directory and NAT Relay

A Node behind NAT makes a short mTLS request to Directory, receives a leased Relay assignment, and opens an outbound TLS 1.3 + HTTP/2 ARPX connection to that Relay. Directory assigns transport; it does not directly trust remote Agent routes.

```mermaid
sequenceDiagram
  participant N as "Router behind NAT"
  participant D as "Directory"
  participant R as "Relay"
  N->>D: mTLS Relay assignment request
  D-->>N: Relay endpoint + bounded lease
  N->>R: outbound TLS 1.3 / HTTP2 ARPX
  R-->>N: Relay session and shared Invoke tunnel
```

```text
DNS-SD / SVCB candidate
        ↓ manual or policy admission
dynamic ARPX Peer
        ↓ leased remote capabilities
ARIB candidates
        ↓ identity policy, hard constraints, scoring
AFIB route
        ↓
Invoke
```

Revoking a Peer withdraws learned capabilities immediately or through controlled graceful expiry. An automatically admitted candidate can return on the next refresh, so also change the admission mode or allowlist.

Self-hosted OpenWrt Open Mesh and Cloud Relay have separate connection state and trust configuration. See [Cloud Relay](../guides/cloud-relay.md) for Cloud (including Community), or [node roles](../guides/router-roles.md) for self-hosted seeds.

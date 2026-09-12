---
sidebar_position: 3
title: Quick Setup
---

# Quick Setup

This page covers a common single-domain deployment. ARPX, SVCB, public IPv6, and capacity controls remain under [Advanced Settings](advanced-settings.md).

| Group/field | Purpose | Recommendation |
| --- | --- | --- |
| Enable Agent routing | Starts or stops the Agent routing plane | Enable for normal operation |
| Router ID | Stable 1–64 character lowercase ID using letters, digits, `.`, `_`, `-` | Unique per Router; avoid later changes |
| Agent domain | Administrative domain | Share only across an intended trust boundary |
| Discover Agent routers on LAN | Consumes `_agent-router._tcp.local` DNS-SD | Enable only for other Routers |
| Publish this router on LAN | Publishes this Router's DNS-SD record | Enable when it should be discovered |
| LAN admission | Manual (`off`), same-domain, or allowlist | Select Managed peer trust before manual approval |
| Router allowlist | Router IDs eligible for automatic admission | One stable ID per entry |
| Connect to an OpenWrt Open Mesh seed | Enables NAT bootstrap | Not needed on a normal LAN |
| Open Mesh Directory URLs | Up to four ordered HTTPS failover URLs | Use operator URLs; hostname is TLS identity |

LAN discovery creates candidates only. Unless automatic policy admits one, review it in [Peer Trust](peer-trust.md).

This page controls `open_mesh_relay_enabled` and `open_mesh_directory_endpoints`, never Cloud Relay. Open Mesh uses up to four `/v1/open-mesh/assignment` endpoints; Cloud connections use Developer mode → Nexus Cloud. Router Mesh defaults to Open; switch to Managed peer trust before using a manual-approval workflow.

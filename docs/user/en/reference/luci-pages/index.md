---
sidebar_position: 1
title: Pages and workflow
---

# LuCI pages and workflow

The menu is **Status → Agent Routing**, opening **User mode** by default. Use Cloud connection, Router network and Agent services for common controls. Detailed pages below are under **Developer mode**. The **Nexus Cloud** page controls address, pairing and Direct/Relay transport; see the [connection guide](../../guides/cloud-relay.md).

| Type | Pages | Use |
| --- | --- | --- |
| Private-cloud initialization | [Quick Setup](quick-setup.md), [Router Roles](roles.md) | Create the trust domain and first node, or host Relay/Directory |
| Feature configuration | [Agent APIs & Protocols](protocols.md), [Advanced Settings](advanced-settings.md), [Static Peers](static-peers.md), [Policy RIB](policy-rib.md) | Join Agents; configure public IPv6, discovery, Peers, and policy |
| Status and trust | [Overview](overview.md), [Local Agents](local-agents.md), [Capability Routes](capability-routes.md), [Neighbors & Discovery](neighbors.md), [Peer Trust](peer-trust.md) | Verify nodes, leases, routes, neighbors, and trust |

Recommended order: **User mode → Developer mode (as needed) → Local Agents → Capability Routes → actual invocation**. Use Quick Setup / Advanced Settings for identity or managed trust changes.

:::tip Saved is not running
Click **Save & Apply** after a change. A valid saved candidate is only the first check; verify services, sessions, leases, and routes on the status pages.
:::

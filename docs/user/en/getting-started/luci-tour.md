---
sidebar_position: 3
title: LuCI interface tour
---

# LuCI interface tour

The current menu is **Status → Agent Routing**, opening **User mode** by default. Legacy direct Overview/Quick Setup URLs redirect to the user home. Expand **Developer mode** for detailed configuration.

## User mode

The home page shows Nexus Cloud, This Router, Neighbor Routers and Agents, with three switches:

| Feature | Purpose |
| --- | --- |
| Cloud connection | Pair Cloud and synchronize eligible Agents |
| Router network | Discover neighbors, establish Peers and exchange capability routes |
| Agent services | SDK registration and Agent invocation |

Use **Pair with Nexus Cloud** for pairing. A new code replaces an existing registration. Read-only accounts can inspect status but cannot change configuration.

## Developer mode

Quick Setup controls identity and discovery. Nexus Cloud controls Cloud address, pairing and Direct/Relay transport. Router Roles controls self-hosted services. Agent APIs & Protocols controls SDK, protocols and authentication; Local Agents and Capability Routes expose detailed leases and AFIB.

Overview, Neighbors & Discovery and Peer Trust help diagnose services, sessions and admission. Advanced Settings, Static Peers and Policy RIB provide detailed controls. See the [page reference](../reference/luci-pages/index.md).

After saving, inspect runtime status and make a real invocation. Green cards do not prove remote calls, public IPv6 or Cloud pairing have succeeded.

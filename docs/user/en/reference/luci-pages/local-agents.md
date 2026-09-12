---
sidebar_position: 5
title: Local Agents
---

# Local Agents

The page groups active local capability leases by Agent identity, endpoint, and tenant. “Connected” normally means renewable leases, not a permanent application socket.

| Column/action | Meaning |
| --- | --- |
| Agent | Agent URI/identity |
| Tenant / endpoint | Tenant and SDK callback endpoint |
| Lease state | Health and earliest expiry |
| Capabilities | Intent, version, route ID, health, and remaining lease |
| Search / state filter | Filter by identity, tenant, endpoint, capability, and health |
| Show native Agent lease metadata | Bounded native JSON for troubleshooting |
| Copy IPv6 connection | Copies a direct descriptor for a router-managed `/128` |

Copy is available only when public descriptor publishing is complete. HTTPS also requires certificate identity and CA bundle label. Addresses bind to capability route IDs; do not assume one permanent `/128` per Agent.

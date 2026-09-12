---
sidebar_position: 12
title: Policy RIB
---

# Policy RIB

Policy filters ARIB candidates by identity and controls AFIB selection. Rules compile atomically; a failed candidate leaves the last valid policy active.

## Defaults and health dampening

| Field | Purpose | Default |
| --- | --- | --- |
| Default action | Allow or deny when no rule matches | allow |
| Failure threshold | Consecutive failures before unhealthy | 3 |
| Recovery threshold | Consecutive successes before recovery | 2 |

## Ordered rule fields

| Field | Purpose |
| --- | --- |
| Enabled / Policy ID / Priority | Switch, stable ID, and sortable priority |
| Action | Allow or deny on match |
| Tenant / Source agent / Intent class | Identity selectors; `*` means any |
| Required region | Accept only a region |
| Route sources | Comma list such as `local,static,peer` |
| Maximum cost / latency | Hard bounds |
| Minimum trust | 0–100 trust floor |
| Maximum load | 0–1000‰ load ceiling |
| Maximum hops | 0–32 path-length ceiling |
| Preferred peer | Preference among otherwise eligible routes |
| Allowed endpoint prefix | Callback URL restriction, commonly `https://` |

Place narrow high-priority denies before broad allows. Validate tenant/intent-specific rules before changing the default action, then confirm the Policy RIB card and Capability Routes.

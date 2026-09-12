---
sidebar_position: 6
title: Capability Routes
---

# Capability Routes

Shows up to 200 ARIB routes selected into AFIB.

| Normal column | Meaning |
| --- | --- |
| Intent | Capability name and version |
| Origin / next hop | Origin Agent and local, Peer, or Relay hop |
| State | Health and hop count |
| Details | Route ID, source, path, endpoint, lease, and raw JSON |

**Advanced columns** add route ID/source, tenant/endpoint, latency, cost, trust, and load. Search by text, source, and health. The advanced-column preference is browser-local and does not modify the Router.

A route does not guarantee a successful call; ingress authentication, callback reachability, mappings, and timeouts still apply. Policy and scoring select only the best eligible candidate into AFIB.

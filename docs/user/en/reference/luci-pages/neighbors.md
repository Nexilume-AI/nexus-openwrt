---
sidebar_position: 7
title: Neighbors & Discovery
---

# Neighbors & Discovery

This read-only page separates admitted Peers from discovery candidates.

| Section | Important columns | Meaning |
| --- | --- | --- |
| ARPX neighbors | Router/peer, endpoint, transport/state, learned routes/sequence | Configured or promoted Peer sessions |
| LAN DNS-SD candidates | Router/domain, LAN address, interface, admission, lease | Multicast-discovered Router candidates |
| Cross-domain SVCB candidates | Router/domain, target:port, DNSSEC, admission, lease | DNSSEC-validated cross-domain candidates |

**Promoted** is an admitted dynamic Peer, **Eligible** only satisfies auto-admission rules, and **Observed** is discovery alone. Discovery never creates capability routes by itself. Review trust, then verify the ARPX session and learned routes.

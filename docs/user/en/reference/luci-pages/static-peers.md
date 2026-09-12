---
sidebar_position: 11
title: Static Peers
---

# Static Peers

This inventory is only for explicit ARPX Peers. LAN-, Agent Card-, and Directory-managed dynamic Peers are not written here.

| Field | Purpose/constraint |
| --- | --- |
| Enabled | Loads the entry |
| Peer ID | Local stable Peer ID, 1–64 restricted characters |
| Router ID | Remote stable Router identity |
| Domain | Remote administrative domain, hostname syntax |
| ARPX endpoint | HTTPS URL such as `https://router.example:7444/arpx/v1` |
| Underlay IPv4 | Optional fixed address; empty uses DNS |
| Role | `peer`, `reflector`, or `relay` |
| Restart grace | 5–300 seconds, default 30 |

A fixed IPv4 overrides only the transport address; HTTPS still validates the endpoint hostname. Do not duplicate a dynamic candidate as a static Peer because identity and lifecycle can conflict.

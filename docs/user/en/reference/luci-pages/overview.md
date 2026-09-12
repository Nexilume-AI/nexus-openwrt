---
sidebar_position: 2
title: Overview
---

# Overview

This read-only page polls bounded metadata every five seconds. It does not read prompts, tool arguments, model output, tokens, or task payloads.

| Card/section | Meaning | If unexpected, check |
| --- | --- | --- |
| AFIB routes | Callable best routes and capacity | Agent leases, policy, static routes |
| ARPX sessions | Established/configured Router Peers | TLS, endpoints, Static Peers, Peer Trust |
| LAN candidates | Current candidates and discovery state | DNS-SD, multicast, interface, firewall |
| Relay tunnels | Established tunnels and assignment state | URL, certificate, egress, lease |
| Public Agent IPv6 | Active `/128` leases and capacity | Prefix routing, NDP, `agent-netd` |
| Recovery | Whether configuration domains loaded | Last error and system log |
| Route index | Index state and bucket count | `agentd` startup and memory bounds |
| Policy RIB | Compiled rules and default action | Policy RIB and atomic reload errors |
| Route memory | Bounded AFIB memory | Capacity and unexpected growth |
| Recovery domains | Reloads, failures, and last error per domain | Fix the named domain before restarting |

Green means that control-plane component is available, not that every business call succeeds. Verify [Capability Routes](capability-routes.md) and make a real end-to-end call.

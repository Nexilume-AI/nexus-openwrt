---
sidebar_position: 2
title: "One Agent, one IP: address ownership"
description: Compare SDK Host Alias, OpenWrt-managed IPv6, and one-address-many-ports models.
---

# One Agent, one IP: address ownership

The core Nexus network model is that **every Agent can have its own IPv6 address**. The network-layer destination identifies the Agent directly instead of asking a host address and port to stand in for process identity.

```text
[Agent A IPv6]:9443  → agent://demo/agent-a
[Agent B IPv6]:9443  → agent://demo/agent-b
```

There are two one-Agent-one-IP paths. The Agent host can own addresses through SDK Host Alias, or OpenWrt can manage Agent ingress addresses. Use one address with many ports only when there is not enough IPv6 address space.

## Path 1: SDK Host Alias

The host leases a distinct `/128` to each Agent from a genuinely usable `/64`. Agents may share a port:

```text
[2001:db8:1::101]:9443 → Agent A
[2001:db8:1::102]:9443 → Agent B
```

`2001:db8::/32` above explains the topology only. A live deployment must use a real global prefix.

`nexus-agent-addressd` performs privileged address operations while ordinary Agents allocate, confirm, renew, and release leases over local IPC. The `/64` must be on-link or routed to the host. A single provider `/128` cannot be expanded into more addresses.

This path supports **Agent-to-Agent calls with only the Python SDK**. It does not use Router ARIB/AFIB, Directory, Relay, capability registration, or callback control planes. Run [Two IPv6 Agents calling each other](https://nexilume-ai.github.io/nexus-docs/en/sdk/tutorials/ipv6-agents-call-each-other) for the complete example.

## Path 2: one OpenWrt-managed IPv6 per Agent

A Python Agent registers with `public_ipv6="auto"`, and OpenWrt allocates a `/128` from a usable `/48`–`/64`. Remote callers connect to the common ingress port, normally `7443`. The Router reads the original destination address and performs an exact route lookup. Failure never falls back to another Agent.

From the user perspective, this is one Router-managed IPv6 per Agent. In the current implementation, the precise binding object is a **local capability-route lease**. An Agent exposing several capabilities must not assume they automatically share one permanent address. This distinction matters for audit, renewal, and recovery.

Addresses come from either:

- **routed-prefix**: an upstream delegates or statically routes a prefix to OpenWrt; preferred;
- **upstream-relay**: without PD, allocate from the WAN on-link `/64` and let `agent-netd` proxy NDP only for live leases. This is NDP proxying, not Nexus cross-NAT Relay.

Choose this path not because the SDK cannot call directly, but because you need centralized TLS/JWT, connection limits, capability routing, protocol adaptation, discovery, or cross-site policy.

## Compatibility path: one IPv6, a different port per Agent

When a host has only one stable public IPv6 address, each Agent listens on a distinct port:

```text
[2001:db8::20]:9441  → Agent A
[2001:db8::20]:9442  → Agent B
[2001:db8::20]:9443  → Agent C
```

The Agent host owns the address and OpenWrt performs ordinary IPv6 forwarding. Deployment is simple, but the port becomes part of service identity and must be preserved in firewall, DNS, and operations records.

## Choose a model

| Property | SDK Host Alias | OpenWrt-managed `/128` | One address, many ports |
| --- | --- | --- | --- |
| One address per Agent | Yes | Yes, currently bound per capability-route lease | No |
| Address owner | Agent host | OpenWrt | Agent host |
| Call path | direct literal IPv6 | exact Router ingress and AFIB | host address plus port |
| Same port for many Agents | Yes | Yes | No |
| Prefix requirement | host has usable `/64` | `/48`–`/64` routed to OpenWrt, or WAN on-link `/64` | one reachable address |
| Automatic discovery | no; add SVCB/Card/Directory | can combine LAN, SVCB, Peer, Directory | no; maintain port records |
| Central ingress policy | host implements it | Router TLS/JWT, connection limits, protocol adapter | host implements it |
| Best fit | SDK-only labs, edge hosts, direct mesh | Agent private cloud, cross-site governance | compatibility when addresses are scarce |

## An address is not authorization

A dedicated IPv6 address answers which Agent receives a request. It does not prove that the caller is trusted. Prefer HTTPS and verified caller identity on public ingress. Plain HTTP is only for isolated labs because it exposes tokens, Envelopes, requests, and results. The SDK never falls back to HTTP after a TLS failure.

Use the [communication mode selector](model.md) to choose SDK direct calls, LAN, DNS SVCB, Router-managed ingress, or NAT Relay.

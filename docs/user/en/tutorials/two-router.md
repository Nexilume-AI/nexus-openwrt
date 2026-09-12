---
sidebar_position: 1
title: Connect two LAN routers
---

# Connect two LAN routers

This lab publishes Router A's Agent capability to Router B over the same layer-2 LAN. Cloud, Relay and Directory are unnecessary for this direct experiment.

## Network and identity prerequisites

- Two OpenWrt devices with compatible Nexus packages and distinct Router IDs: `router-a` and `router-b`.
- LAN DNS-SD/umDNS discovery and ARPX connectivity; check TCP `7444` and both Peer TLS configurations.
- Non-conflicting management addresses. Do not connect two default DHCP-serving LANs without planning. Keep one intended DHCP service, or use static addresses.
- At least one registered Agent that Router A can already invoke locally.

The desktop launcher has a fixed VM name, switch and IPv4/ULA subnet and supports one instance. Running it twice does not create this topology. Use separate devices or lab VMs with explicitly distinct identities, addresses and switches; see [desktop VM](../getting-started/desktop-vm.md).

## Path A: default Open Mesh

1. Enable **Router network** on both devices in **Status → Agent Routing → User mode**. Enable **Agent services** on A.
2. Check unique IDs in **Developer mode → Quick Setup**.
3. Confirm **Open distributed mesh (zero configuration)** in **Advanced Settings → Router Mesh** on both sides.
4. Wait for the other device under Neighbor Routers. Default automatic admission does not require matching domains or individual approval.
5. On B, confirm an ARPX session in **Developer mode → Neighbors & Discovery**, then look for A's capability in **Capability Routes**.

## Path B: manual approval lab

First select **Managed peer trust** in **Advanced Settings → Router Mesh** on both devices, then choose **Quick Setup → LAN admission → Manual approval**. Both certificate chains and TLS identities must satisfy their managed trust configuration; toggling the mode does not establish a shared CA.

Review the other Router's ID, domain and endpoint in **Peer Trust** on each side. Approve B on A and A on B; do not assume one approval establishes bidirectional publication. An automatically admitted Peer can return after revocation while its automatic admission source remains enabled.

## Verify invocation and withdrawal

Confirm a live lease from A on B, then invoke through B's configured API. Point the caller's SDK at B; A must be able to reach the Agent callback. A host-only `127.0.0.1` listener is unsuitable.

A successful response verifies discovery, session, route, policy and forwarding together. Stop A's Agent afterward and confirm that withdrawal or expiry removes it as a callable target on B.

## Troubleshooting

- **No neighbor:** check layer-2 isolation, umDNS, discovery/publication and address conflicts.
- **Candidate without session:** check Mesh mode, admission on both sides, TCP 7444, certificates and clock. Missing approval is not the only cause.
- **Session without capability:** check A's lease, import/export policy and B's AFIB.
- **Across NAT/sites:** choose a [self-hosted Open Mesh seed](../guides/router-roles.md) or [Cloud Relay](../guides/cloud-relay.md); never mix the assignment settings.

---
sidebar_position: 4
title: Extend an Agent private cloud by scenario
---

# Extend an Agent private cloud by scenario

## Scenario 1: one-node Agent private cloud

1. Follow [Configure an Agent private cloud network](../getting-started/quick-setup.md) to set Router ID and Agent domain.
2. Enable Python SDK registration and Agent calls under **Agent APIs & Protocols**.
3. Keep **Automatically call registered LAN Agents** enabled.
4. Bind No-JWT mode to trusted LANs only; use JWKS for Internet-facing access.
5. Register each Agent's private IPv4 or ULA callback. The Router creates and removes mappings with the lease.

One node already provides registration, routing, and invocation. Router-to-Router LAN discovery is unnecessary until a second Nexus Router exists on the LAN.

## Scenario 2: extend to several nodes on one LAN

1. Give every node a unique Router ID; use the same Agent domain only when same-domain admission is intended.
2. Publish and consume LAN DNS-SD.
3. Default Open Mesh automatically admits validated neighbors. For manual approval, first switch Router Mesh to Managed peer trust, then select Manual approval and inspect Peer Trust.
4. In Managed peer trust, select same-domain or allowlist according to your intended boundary.
5. Check the ARPX session in **Neighbors & Discovery**, then remote capabilities in **Capability Routes**.

## Scenario 3: join one public Agent host

- With one public IPv6 address, assign a different port to each Agent.
- With a usable `/64`, use Host Alias for one `/128` per Agent and optionally reuse a port.
- In both cases the host owns TLS, authentication, and firewall policy; the Router only forwards IPv6.

## Scenario 4: let OpenWrt manage public Agent addresses

1. Verify that a `/48`–`/64` is routed to OpenWrt, or that WAN has an on-link `/64` when no PD is available.
2. Choose the source and enable allocation under **Advanced Settings → Public Agent IPv6**.
3. In **Public IPv6 ingress readiness**, select HTTPS mTLS, allow invocation, set port `7443`, and bound connections.
4. Enable the adapter and explicit mappings when MCP/A2A is required.
5. Register the Agent with `public_ipv6="auto"`; copy its descriptor from **Local Agents**.

Never deploy documentation prefix `2001:db8::/32`. Test routing to a leased `/128` from an external network, not only from the Router itself.

## Scenario 5: connect cross-domain nodes through SVCB

1. Publish `_agents.<domain>` SVCB with a valid DNSSEC chain.
2. Run a local validating resolver; prefer automatic detection.
3. Enter the remote administrative domain under **Advanced Settings → Cross-domain discovery**.
4. Review DNSSEC and Agent Card/Directory evidence in **Peer Trust** before admission.
5. Verify the Peer session and capability route on their separate status pages.

## Scenario 6: extend the private cloud across NAT

1. Enable self-hosted Open Mesh Relay in **Quick Setup** or **Advanced Settings → Open Mesh Relay**.
2. Enter up to four seed-provided HTTPS `/v1/open-mesh/assignment` URLs in failover order.
3. Hostnames remain the SNI and certificate identity; a fixed IPv4 is only an underlay override.
4. Confirm an active assignment, Relay tunnel, and then remote routes in **Overview**.

Normal NAT nodes remain **Node only**. Select a Relay or Directory server role under **Router Roles** only when this node provides that service to others.

## Scenario seven: Cloud Community

Community startup includes Cloud Relay. Pair the Router in **Developer mode → Nexus Cloud** and use Auto or Relay only; no self-hosted Directory URL is required in Quick Setup. See [Cloud Relay](../guides/cloud-relay.md) for server IP, device mTLS and verification.

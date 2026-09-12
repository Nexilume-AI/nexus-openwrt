---
sidebar_position: 2
title: Configure an Agent private network
---

# Configure an Agent private network

The current entry is **Status → Agent Routing → User mode**. It controls Cloud connection, Router network and Agent services. Identity, trust and protocol settings live under **Developer mode**.

## 1. Check node identity

Open **Developer mode → Quick Setup** and check Router ID and Agent domain. Each Router ID must be unique and stable: 1–64 lowercase letters, digits, dots, underscores or hyphens, starting and ending with a letter or digit. Never deploy two cloned devices with the same identity.

Agent domain identifies a management domain. The current default is **Router Mesh = Open distributed mesh (zero configuration)**, which can exchange routes across domains. A matching domain name is not the default authorization boundary.

## 2. Enable the required features

Return to **User mode**:

- **Agent services** enables SDK registration and Agent invocation.
- **Router network** enables Peer transport/listening and LAN discovery/publication. Disable it for an isolated single-node experiment when neighbors are unnecessary.
- **Cloud connection** is for an installation with a Cloud account, HTTPS ingress and pairing code. Local Agent networking also works without Cloud.

A switch being enabled does not prove that Agents or remote routes exist. Use **Developer mode → Agent APIs & Protocols** for explicit listener and authentication settings.

## 3. Choose neighbor trust

Default Open Mesh automatically admits validated LAN, static-seed and DNSSEC/SVCB Router discoveries without per-peer approval. Discovery does not create Agent capabilities: a Peer must publish leases, and route policy still selects usable routes.

For individual approval, choose **Managed peer trust** in **Developer mode → Advanced Settings → Router Mesh**, then set **LAN admission → Manual approval** in Quick Setup. Configure both sides intentionally and inspect candidates in **Peer Trust**. Changing LAN admission alone does not turn Open Mesh into managed trust.

## 4. Distinguish the two Relay paths

- For Nexus Cloud, including Community with bundled Relay, use pairing and **Cloud connectivity** in **Developer mode → Nexus Cloud**. See [Cloud and Cloud Relay](../guides/cloud-relay.md).
- For a self-hosted OpenWrt seed, enable **Connect to an OpenWrt Open Mesh seed** in Quick Setup and supply up to four **Open Mesh Directory URLs**. Copy the complete seed-provided endpoint ending in `/v1/open-mesh/assignment`.

These settings are independent; Quick Setup never changes Cloud Relay. Normal clients keep **Router Roles → Hosted services → Node only**.

## 5. Verify registration and invocation

Inspect Agents, Neighbor Routers and routes in User mode. Detailed views are **Developer mode → Local Agents / Capability Routes / Overview**. Verify a real registration, lease and invocation. Zero Peer/Relay sessions are normal for an isolated node.

Local experiments use IPv4/ULA callbacks reachable from the Router. Public Agents have different address prerequisites; see [IPv6](../guides/ipv6.md) and [publishing APIs](../guides/publish-api.md).

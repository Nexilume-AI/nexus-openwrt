---
slug: /
sidebar_position: 1
title: OpenWrt User Guide
description: Start by configuring an Agent private cloud network, then learn communication modes, LuCI, networking principles, and deployment labs.
---

# OpenWrt User Guide

Nexus Agent Router turns OpenWrt into an Agent private-cloud node. Agents publish leased capabilities; callers discover and invoke them; the Router enforces identity, trust, policy, selection, and forwarding. Use the guide as task reference or read it as a textbook from principles through labs.

## Configure an Agent private cloud network

For a computer-based experience, see the [full OpenWrt VM](getting-started/desktop-vm.md). The launcher and build recipe are available; a preinstalled image and boot acceptance remain pending. Physical routers retain their device installation path.

1. [Choose a deployment path](getting-started/choose-path.md), then [create the trust domain and first node](getting-started/quick-setup.md).
2. Use the [communication mode selector](communication/model.md) to choose address ownership, data path, and discovery.
3. Check every screen and field in the [LuCI page reference](reference/luci-pages/index.md).
4. Publish a [first Python Agent](https://nexilume-ai.github.io/nexus-docs/en/sdk/quickstart/first-agent), then inspect [Local Agents](reference/luci-pages/local-agents.md) and [Capability Routes](reference/luci-pages/capability-routes.md).

A single node is already a working minimum private cloud. Add LAN Peers, SVCB, Relay, Directory, or public IPv6 only when extending its boundary.

## Textbook path

| Unit | Question it answers |
| --- | --- |
| [Two-plane mental model](concepts/mental-model.md) | Why are IP routing and Agent routing separate? |
| [Capabilities, routes, and leases](concepts/capability-lifecycle.md) | How does a route appear, renew, and disappear? |
| [ARIB to AFIB](concepts/routing-selection.md) | Where do authorization, constraints, and scoring apply? |
| [Invoke data path](concepts/invoke-data-path.md) | What security and process boundaries does HTTP/MCP/A2A cross? |
| [Communication mode selection](communication/model.md) | When should Agents use ports, individual `/128`s, or Router-managed addresses? |
| [Discovery, trust, and Relay](communication/discovery.md) | What do LAN, SVCB, Directory, and Relay each solve? |
| [Reliability and observability](concepts/reliability-observability.md) | How do failures converge without blindly restarting a task? |

Reinforce the model with the [route lifecycle](tutorials/route-lifecycle-lab.md), [policy routing](tutorials/policy-routing-lab.md), and [two-Router](tutorials/two-router.md) labs.

## Enter by task

- LAN, multiple nodes, public IPv6, SVCB, or NAT: start with [scenario recipes](communication/scenarios.md).
- SDK, HTTP/SSE, MCP, and A2A: read [Publish an Agent API](guides/publish-api.md) and [Agent APIs & Protocols](reference/luci-pages/protocols.md).
- Public `/128` and ingress authentication: read [address ownership](communication/addressing.md) and [Advanced Settings](reference/luci-pages/advanced-settings.md).
- Relay/Directory servers: [configure private-cloud node roles](guides/router-roles.md).
- Runtime trouble: begin at the [diagnostics center](troubleshooting/diagnostics.md).

The current OpenWrt package line is **3.1**; see [package reference](reference/packages.md) for component versions.

The current LuCI home is **Status → Agent Routing → User mode**, with detailed settings under Developer mode. Cloud Community also provides Relay; see [Cloud Relay](guides/cloud-relay.md).

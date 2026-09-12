---
sidebar_position: 4
title: Connect Cloud and Cloud Relay
---

# Connect Cloud and Cloud Relay

Cloud Relay carries an outbound Router connection to Nexus Cloud. It is managed separately from a self-hosted OpenWrt Open Mesh seed. Ordinary client routers need no Relay server package and keep **Router Roles → Hosted services → Node only**.

## Prepare Cloud

The enterprise integrated launcher and current Community startup both include Cloud Relay. See [Community startup](https://nexilume-ai.github.io/nexus-docs/en/server/getting-started/quickstart). Community defaults to local-only Relay access; its operator must select a Router-reachable IP on first start and expose the intended tunnel listener. Default tunnel port: `27444`. Internal invoke port `27445` must not be published.

Cloud reporting Relay available does not prove a Router is connected. Verified Cloud HTTPS, device-mTLS ingress, an owner/administrator-issued pairing code and outbound connectivity are still required. The Community test URL `http://127.0.0.1:18090` cannot serve as a remote Router's Cloud URL.

## Pair the Router

1. Open **Status → Agent Routing → User mode**, enter a code and select **Pair with Nexus Cloud**.
2. For an explicit server address or transport mode, open **Developer mode → Nexus Cloud**.
3. Supply the HTTPS **Nexus Cloud URL** and **One-time pairing code**. A new code on an enrolled device replaces its registration.
4. Keep **Certificate management → Managed by Nexus Cloud**. The private key stays on the Router; only a CSR is sent. The code is removed after success.
5. Choose **Cloud connectivity → Auto: Direct IPv6, then Relay**, or explicitly **Relay only (NAT / no IPv6)** when needed.
6. After Save & Apply, run **Check Device TLS** to inspect certificates, matching keys, expiry and Cloud TLS/JWT/JWKS status.

Auto prefers Direct IPv6 when the public TLS descriptor is healthy. Relay uses an outbound mTLS tunnel without opening a Router WAN ingress port. Server identity verification remains required.

## Verify and keep the paths separate

Check pairing, Cloud Relay and Agent synchronization on the Nexus Cloud page, then invoke a synchronized capability from Console. A saved form or reachable local LuCI page does not establish this result.

Quick Setup's **Open Mesh Directory URLs** and `/v1/open-mesh/assignment` from Router Roles belong to the separate self-hosted path. Never put a Cloud URL or Cloud Relay tunnel URL there, or change self-hosted roles to repair Cloud pairing.

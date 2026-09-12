---
sidebar_position: 10
title: Advanced Settings
---

# Advanced Settings

This page writes `agent`, `agent_gateway`, and `agent_adapter`. `agentd` rejects an invalid candidate atomically and preserves the active configuration. Prefer Quick Setup for normal deployments.

## Source defaults

Values below come from current `agentd.config`. UCI defaults, User mode actions and retained upgrades can change effective values; inspect the device with `uci show agent`. `router_mesh_mode` defaults to `open`; `off` means Managed peer trust.

## Identity & capacity

| Field | Purpose | Default/range |
| --- | --- | --- |
| Enable agentd | Core control plane | Enabled |
| Router ID / Agent domain | Stable identity and administrative domain | `router-local` / `local.invalid` |
| Maximum ARIB routes | Candidate route capacity | 10000; 1–100000 |
| Default lease | Lease when none is specified | 30 seconds; 5–3600 |

## ARPX transport

| Field | Purpose | Default/risk |
| --- | --- | --- |
| Enable outbound peer transport | Connect to Peers/Relay | On |
| Enable inbound peer listener | Accept ARPX from Routers | On; configure TLS/firewall |
| Reflect learned routes | Propagate learned routes | Off; not for normal edges |
| Listen IPv4 / port | Inbound ARPX listener | `0.0.0.0:7444` |
| Maximum inbound sessions | Connection bound | 8; 1–128 |
| Enable shared Invoke tunnel | Carry Invoke on ARPX/Relay sessions | On; both sides must agree |

## LAN discovery

| Field | Purpose | Default/range |
| --- | --- | --- |
| Consume LAN DNS-SD | Discover Routers | On |
| Publish router DNS-SD record | Make this Router discoverable | On |
| Zero-configuration admission | off, same-domain, allowlist, all | all; Open Mesh automatically admits peers; configure managed trust separately |
| Router allowlist | IDs for allowlist mode | One per item |
| Graceful restart | Auto-admitted Peer grace | 30 seconds; 5–300 |

## Cross-domain discovery

| Field | Purpose | Default/condition |
| --- | --- | --- |
| Enable DNS SVCB discovery | Query `_agents.<domain>` | Off |
| Remote domain | Remote Router admin domain, not Agent ID/URL | `remote.invalid` |
| Local DNSSEC resolver | Read-only effective validator | Must provide authenticated data |
| Resolver configuration | auto detects Unbound/dnsmasq; manual overrides | auto |
| Resolver IPv4 / port | Manual loopback validator | `127.0.0.1:1053`; address must be `127/8` |
| Agent Card authorization | off, same-domain, allowlist, all-signed, directory-trusted | off |
| Agent Card router allowlist | Router IDs | Only in allowlist mode |

`all-signed` applies local signature policy; `directory-trusted` additionally requires a verified, unexpired Directory trust bundle.

## Open Mesh Relay

| Field | Purpose | Default/range |
| --- | --- | --- |
| Enable self-hosted Open Mesh Relay | Obtain a Relay lease | Off |
| Open Mesh Directory URLs | Up to four ordered HTTPS URLs | Hostname is TLS identity |
| Optional fixed Directory IPv4 | Positional underlay override | Empty uses DNS |
| Directory timeout | Request timeout | 5000 ms; 100–60000 |
| Require forwarding assertions | Require trusted source-Router delegation | Off; useful for trusted cross-domain deployments |

## Public Agent IPv6

| Field | Purpose | Default/condition |
| --- | --- | --- |
| IPv6 address source | auto, routed-prefix or upstream-relay | auto |
| Assign public IPv6 addresses | Allows `public_ipv6="auto"` | On (valid upstream still required) |
| Detected routed/PD prefix | Read-only `/48`–`/64` | Route a prefix or choose no-PD mode |
| Detected upstream on-link `/64` | Read-only WAN prefix | Required for upstream-relay |
| Agent IPv6 source prefix | Allocation source | `/48`–`/64`; exactly `/64` for upstream-relay |
| Upstream IPv6 interface | NDP proxy logical interface | `wan6`; upstream-relay only |
| Maximum active Agent addresses | `/128` lease capacity | 256; 1–65535 |
| Dedicated virtual interface | Address attachment | `nexus-agent0`; does not alter WAN/default route |

## Static capability routes

| Field | Purpose |
| --- | --- |
| Enabled / Route ID | Switch and stable hexadecimal ID |
| Intent / Version | Capability and unsigned version |
| Origin agent / Invoke endpoint | Agent URI and callback URL |
| Tenant / Region | Policy and scoring metadata |
| Cost / Latency / Trust / Load / Hop count | Cost, ms, 0–100, 0–1000‰, 0–32 |

Static routes do not renew or expire. Do not use them as a substitute for SDK registration.

## Public IPv6 ingress readiness

| Field | Purpose | Recommendation |
| --- | --- | --- |
| Direct IPv6 transport | disabled, HTTPS mTLS, plain HTTP, legacy auto | mTLS publicly; HTTP only in isolated labs |
| Allow Agents to call other Agents | Enables Invoke | Required for public calls |
| Agent call authentication | No JWT or JWT | Prefer JWT publicly; it may complement mTLS |
| Allow streaming and reconnect | SSE and same-route resume | Enable for long tasks |
| Public listener address | mTLS listener such as `[::]:7443` | Match certificate/fw4 |
| Destination port on every `/128` | Common managed-address port | 7443 |
| Publish direct descriptor | Adds scheme/port/TLS metadata to registration | Enable for external direct clients |
| TLS certificate identity | Exact DNS SAN name | HTTPS descriptor only |
| Caller CA bundle label | Caller-local CA mapping label, not a path | 1–63 safe characters |
| Maximum concurrent public connections | Ingress connection bound | 32; 1–128 |

Plain HTTP exposes JWTs and payloads and is never a TLS-failure fallback. The destination `/128` always binds exactly one local route.

## MCP and A2A public ingress

| Field | Purpose |
| --- | --- |
| Enable MCP and A2A adapter | Protocol adapter; normal HTTP/SSE does not require it |
| Allow MCP and A2A streaming | Adapter streaming; gateway streaming is also required |

Configure protocol-to-capability mappings under [Agent APIs & Protocols](protocols.md).

This page controls `open_mesh_relay_enabled` and `open_mesh_directory_endpoints`, never Cloud Relay. Open Mesh uses up to four `/v1/open-mesh/assignment` endpoints; Cloud connections use Developer mode → Nexus Cloud. Router Mesh defaults to Open; switch to Managed peer trust before using a manual-approval workflow.

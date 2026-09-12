---
sidebar_position: 9
title: Agent APIs & Protocols
---

# Agent APIs & Protocols

Configure SDK ingress, authentication, Agent callbacks, and explicit MCP/A2A mappings. The Router never inspects prompts or tool arguments to guess a route.

## Common setup

| Field | Purpose | Default/relation |
| --- | --- | --- |
| Allow Agents to call other Agents | Envelope route-and-invoke | Off; required for calls |
| Allow Python SDK registration | register/renew/unregister | Off; expose only on protected ingress |
| Enable MCP/A2A compatibility | MCP `tools/call` and A2A `message:*` adapter | Off |
| Enable streaming responses | Synchronizes adapter/gateway streaming | Off |
| Resume long-running calls | Retains task-to-route binding for Last-Event-ID | Shown with streaming; on by default |
| Default tenant | Tenant assigned at ingress | `local` |
| Protocol adapter identity | Source Agent URI | `agent://local/adapterd` |

Resume returns to the original Agent and replays retained events; it never restarts the task elsewhere.

## Authentication & task credentials

| Field | Purpose | Condition/recommendation |
| --- | --- | --- |
| Agent call authentication | No JWT, Remote JWKS, Local public key | Prefer Remote JWKS publicly |
| Enable No JWT LAN access | Dedicated trusted-LAN listener | No-JWT mode only; firewall it |
| No JWT LAN listener | Address used in `NEXUS_ROUTER_URL` | `0.0.0.0:7445` |
| Token issuer URL | Exact JWT `iss` | JWKS/local modes |
| Router audience | JWT `aud` | `nexus-agent-router` |
| JWKS URL | Remote keys; derived from issuer when empty | JWKS mode, HTTPS |
| Configuration status | Checks saved settings and services | Save & Apply first |

### Advanced authentication fields

| Field | Purpose | Default/range |
| --- | --- | --- |
| Local signing key ID | Matches token `kid` | `default` |
| Local public key file | Router-local PEM | `/etc/agent-gw/jwt-public.pem` |
| JWKS cache file | Refresher-managed cache | `/etc/agent-gw/jwks.json` |
| JWKS HTTPS CA file | Trust roots for key download | System CA bundle |
| Allowed clock skew | Time tolerance | 30 seconds; 0–300 |
| Maximum token lifetime | Token validity bound | 300 seconds; 1–3600 |
| Transaction-token replay cache | One-time transaction IDs | 512; 1–65536 |

## Python Agent Servers

| Field/button | Purpose | Recommendation |
| --- | --- | --- |
| Automatically call registered LAN Agents | Lease-managed private IPv4/ULA callbacks | Keep enabled |
| Show automatic Agent example | Minimal Python called-Agent sample | Use for first integration |
| Automatically call registered HTTPS Agents | Lease-managed address/TLS name/CA label/fingerprint | Enable for remote HTTPS Agents |
| HTTPS Agent certificate trust | System CA or private/enterprise CA | Administrator pre-trusts it; Agents cannot install CAs |
| Show automatic HTTPS example | HTTPS Agent sample | Use after saving CA mode |
| Trusted CA label | Matches Agent `server_ca_bundle_id` | Label, not certificate |
| Trusted CA bundle file | Administrator-approved local PEM | Agent cannot overwrite it |
| Show legacy fixed mappings | Reveals compatibility/recovery field | Not needed for normal SDK leases |
| Fixed HTTPS endpoint mappings | `TLS-name:port=IPv4/[IPv6]`; dynamic lease wins | Emergency manual use only |

## Advanced limits

| Field | Purpose | Default/range |
| --- | --- | --- |
| Default hop limit | Envelope hop ceiling | 8; 2–255 |
| Call timeout | Adapter-to-gateway timeout | 5500 ms; 100–60000 |
| Concurrent protocol calls | Concurrency bound | 32; 1–1024 |
| Maximum request bytes | Request bound | 65536; 1024–1048576 |
| Maximum response bytes | Response bound | 262144; 1024–4194304 |
| Stream idle timeout | No-event timeout | 15000 ms; 1000–300000 |
| Maximum SSE event bytes | Per-event bound | 65536; 256–1048576 |
| Retained resumable tasks | Task-route binding capacity | 512; 1–4096 |
| Resume window | Replay retention after disconnect/completion | 300 seconds; 1–86400 |

Called Agents should retain event history for at least the Router resume window.

## MCP/A2A capability mappings

| Field | Purpose |
| --- | --- |
| Enabled | Loads the mapping |
| Protocol | MCP `tools/call` or A2A `message:send/message:stream` |
| Server / Agent Card ID | Stable URL authority/Card ID |
| Tool name / A2A Skill ID | Exact selector; A2A must match `agent.expose()` skill |
| Capability intent / version | Nexus route lookup key |
| A2A/MCP caller URL | Generated read-only URL after save |
| Show Python example | Caller/called-Agent sample for the mapping |

The mapping is explicit: `protocol + authority + selector → intent.vN`. Request content never influences routing.

## User mode and LAN SDK

User mode **Agent services** enables registration, invocation, the LAN SDK listener, LAN callbacks and adapter together. The LAN SDK listener defaults to `0.0.0.0:7446`; raw package flag defaults differ from the effective state after enabling this feature. Legacy No JWT LAN listener `7445` is a separate endpoint. Use the enabled listener and its authentication flow, and ensure the Router can reach the host callback address.

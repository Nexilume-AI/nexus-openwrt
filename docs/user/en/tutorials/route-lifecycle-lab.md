---
sidebar_position: 2
title: "Lab: observe a complete route lifecycle"
---

# Lab: observe a complete route lifecycle

Publish a `demo.echo` Agent and observe registration, AFIB appearance, renewal, and withdrawal. You will connect “the Python process started” with “the Router considers it callable.”

## What you need

- Completed OpenWrt [Quick Setup](../getting-started/quick-setup.md).
- A Python 3.9+ host that can reach the Router.
- The SDK and a token with registration/invoke permissions.
- LuCI; SSH/ubus is optional developer-level observation.

## Step 1: record the baseline

Open **Local Agents** and **Capability Routes** in LuCI. Optionally record:

```sh
ubus call agent agents '{"limit":100}'
ubus call agent routes '{"limit":100}'
ubus call agent stats '{}'
```

Note the generation and whether `demo.echo` already exists.

## Step 2: start an Agent and see the first result

```bash
export NEXUS_ROUTER_URL=http://192.168.1.1:7443
export NEXUS_AGENT_TOKEN=replace-with-a-real-token
python examples/friendly_agent.py
```

PowerShell:

```powershell
$env:NEXUS_ROUTER_URL = 'http://192.168.1.1:7443'
$env:NEXUS_AGENT_TOKEN = 'replace-with-a-real-token'
python examples/friendly_agent.py
```

LuCI should now show `agent://demo/echo-server` and `demo.echo`. The listener, registration, and AFIB are connected.

## Step 3: explain the route

```sh
ubus call agent agents '{"limit":100}'
ubus call agent routes '{"limit":100}'
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/caller-1"}'
```

Compare origin, route ID, lease, source, health, and the selected lookup route. The route ID belongs to this lease and is not a permanent Agent ID.

## Step 4: invoke it

Keep the Agent running and complete [Call your first Agent](https://nexilume-ai.github.io/nexus-docs/en/sdk/quickstart/call-first-agent). A real echo response proves the data plane works, not only the control-plane route.

## Step 5: observe withdrawal and expiration

Press Ctrl+C in the Agent terminal. The high-level SDK unregisters, so the route should disappear. A forced process kill has no clean unregister; the Router removes that route when its lease expires. Do not force-kill a production Agent for this lab.

## What you learned

Agents/routes/lookup describe control state, while the echo response proves data flow. Clean exit uses explicit withdrawal; abnormal exit converges through lease expiration. Continue with the [policy-routing lab](policy-routing-lab.md).

## Troubleshooting

- No route after startup: check token scope, Router URL, advertised address, and registration errors.
- Route exists but invoke misses: check tenant, source identity, Policy RIB, and lease.
- Route remains briefly after Ctrl+C: refresh the generation or wait for lease expiration if shutdown was not clean.

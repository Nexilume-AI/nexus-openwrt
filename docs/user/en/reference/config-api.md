---
sidebar_position: 3
title: UCI, ubus, and diagnostics
---

# UCI, ubus, and diagnostics

LuCI writes UCI configuration and controls runtime services through OpenWrt and ubus. Automation should use the project's UCI and ubus interfaces instead of editing generated runtime files.

## Common read-only commands

```bash
uci show agent
uci show nexus_roles
ubus call agent stats
ubus list | grep agent
logread | grep -E 'agentd|agent-gw|agent-adapter'
```

Detailed fields and methods are maintained in the repository's integration references:

- `docs/UCI_CONFIG.md`
- `docs/UBUS_API.md`
- `docs/AUTHENTICATION.md`
- `docs/SECURITY.md`

These files target developers and integrators. Confirm that a field exists in the target firmware before automating a write. Review changes with `uci changes`, then apply them with a service reload or LuCI **Save & Apply**.

Cloud configuration lives in `nexus_cloud` and may contain an unconsumed pairing code; do not dump it into public logs. Self-hosted Open Mesh uses `agent.main.open_mesh_*` fields; the connector manages Cloud Relay separately.

---
sidebar_position: 1
title: Architecture
---

# Architecture

```mermaid
flowchart LR
  C["Client"] --> G["agent-gw"]
  G --> D["agentd control plane"]
  D --> R["Capability routes and policy"]
  R --> A["Local agent"]
  R --> P["Trusted peer router"]
  A --> S["Tool or model service"]
```

The control plane maintains agent leases, capabilities, and peer relationships. The gateway accepts calls and streaming responses. Adapters map different agent protocols to one routing model. LuCI manages configuration and displays runtime state.

A typical call passes through capability registration, route generation, discovery or route selection, authentication and policy checks, forwarding, and response delivery. A route is removed when its lease expires, trust is revoked, or health checks fail.

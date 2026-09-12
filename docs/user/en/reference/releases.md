---
sidebar_position: 0
title: Package acquisition and release batches
---

# Package acquisition and release batches

The repository defines source and OpenWrt SDK builds but does not currently declare a public binary download URL. Before an external release, the publisher should provide a package repository or download page with SHA-256 checksums.

## Build output

After an OpenWrt 25.12 SDK build, project packages are normally under:

```text
bin/packages/<architecture>/nexus_agent_router/
```

The accepted reference target uses `x86_64`. Never install x86/64 packages on ARM or MIPS hardware.

## Recommended package sets

| Use case | Packages |
| --- | --- |
| Base Node | `agent-netd`, `agentd`, `agent-gw`, `agent-adapter`, `nexus-agent-roles`, `luci-app-agent-router` |
| Node + Relay | Base Node + `nexus-agent-relayd` |
| Node + Directory | Base Node + `nexus-agent-directoryd` |
| CLI only | Remove `luci-app-agent-router`; retain runtime components |

Verify files before installation:

```sh
sha256sum *.apk
```

Checksums must come from a trusted channel separate from the package files.

## Version matching

Components in one release batch can use different semantic versions, such as `agentd 3.1.0` and `agent-gw 0.22.0`. Do not mix packages by major version alone. Use the release manifest, build timestamp, and checksums to identify a batch.

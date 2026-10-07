---
sidebar_position: 0
title: Package acquisition and release batches
---

# Package acquisition and release batches

Download signed x86_64 Beta package feeds from [GitHub Releases](https://github.com/Nexilume-AI/nexus-openwrt/releases). Each batch includes its target, package manifest, public signing key and SHA-256 checksums. Follow the [installation guide](../../../package-install.md) and verify the key fingerprint in the release notes before trusting the index.

The first binary batch targets **OpenWrt 25.12.4 x86/64**. It is not a firmware image or an offline installer; matching official repositories supply system dependencies.

## Build output

After an OpenWrt 25.12 SDK build, project packages are normally under:

```text
bin/packages/<architecture>/nexus_agent_router/
```

The accepted reference target uses `x86_64`. Never install x86/64 packages on ARM or MIPS hardware.

## Recommended package sets

| Use case | Packages |
| --- | --- |
| Router | `nexus-agent-router`: routing, LuCI and Cloud connector; no Node.js |
| Router + Relay | `nexus-agent-router-relay`: Router, self-hosted Relay and Node.js |
| Seed | `nexus-agent-router-seed`: Router, self-hosted Relay, Directory and Node.js |

Verify files before installation:

```sh
sha256sum -c SHA256SUMS
```

Compare the signing-key fingerprint through a trusted channel, then verify the signed APK index. Checksums alone do not establish authenticity. Never bypass APK signature validation.

## Version matching

Components in one release batch can use different semantic versions, such as `agentd 3.1.0` and `agent-gw 0.22.0`. Do not mix packages by major version alone. Use the release manifest, build timestamp, and checksums to identify a batch.

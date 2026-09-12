---
sidebar_position: 1
title: Install and build
---

# Install and build

The router components are supplied as an OpenWrt feed. Run the following commands from an OpenWrt source tree or an SDK that matches the target architecture.

## Prerequisites

- A Linux build environment with a working OpenWrt source tree or SDK.
- This repository cloned to a path accessible from the OpenWrt build directory.
- A target device with persistent storage and LuCI or SSH administration.

## Add the feed

Add this repository to `feeds.conf.default`, then update and install its indexes:

```bash
./scripts/feeds update nexus_agent_router
./scripts/feeds install agentd agent-gw agent-adapter
```

Run `make menuconfig` and select the packages you need. A typical installation contains:

- `agentd`: configuration and routing control plane.
- `agent-gw`: Agent HTTP gateway.
- `agent-adapter`: protocol adaptation.
- `luci-app-agent-router`: web administration.

## Build

```bash
make package/agentd/compile \
     package/agent-gw/compile \
     package/agent-adapter/compile V=s
```

You can also use the reproducible build helper from this repository:

```bash
sh scripts/build-openwrt-sdk.sh /absolute/path/to/openwrt-sdk
```

The current OpenWrt 25.12 baseline uses `.apk` packages. Take the matching packages from `bin/packages/`, configure the trusted signed package source and dependencies, then install with `apk add`. Do not apply older `opkg` instructions to this baseline.

## Verify

Refresh the browser and open **Status → Agent Routing → User mode**. If the application is missing, sign in to LuCI again and check:

```bash
apk info | grep -E 'agentd|agent-gw|agent-adapter|luci-app-agent-router'
logread | grep -E 'agentd|agent-gw|agent-adapter'
```

Next: [complete quick setup](quick-setup.md).

---
sidebar_position: 4
title: How to upgrade or uninstall
---

# How to upgrade or uninstall Nexus Agent Router

This guide targets the `apk` package manager in OpenWrt 25.12. Upgrades preserve declared configuration files, but you should still create a backup first.

## Before upgrading

1. Record versions and state:

   ```sh
   apk info | grep -E 'agentd|agent-gw|agent-adapter|luci-app-agent-router|nexus-agent'
   /etc/init.d/agentd status
   /etc/init.d/agent-gw status
   ```

2. Create a system backup:

   ```sh
   sysupgrade -b /tmp/nexus-before-upgrade.tar.gz
   ```

3. Download the backup to an administration computer. It may contain credentials; never attach it to a public issue.

## Upgrade

Upload one matching package batch. Install dependencies and runtime components first, then LuCI:

```sh
apk add --allow-untrusted --upgrade ./agent-netd-*.apk
apk add --allow-untrusted --upgrade ./agentd-*.apk ./agent-gw-*.apk ./agent-adapter-*.apk
apk add --allow-untrusted --upgrade ./nexus-agent-roles-*.apk ./luci-app-agent-router-*.apk
```

Each wildcard must match one intended version. Use full filenames if the directory contains multiple versions.

## Verify

```sh
/etc/init.d/agentd restart
/etc/init.d/agent-gw restart
/etc/init.d/agent-adapter restart
logread | grep -E 'agentd|agent-gw|agent-adapter' | tail -n 80
```

Confirm **Recovery: Healthy** on LuCI **Agent Router → Overview**, then invoke a known capability. Compare any `.apk-new` configuration before applying it; never overwrite identities or keys blindly.

## Uninstall

```sh
/etc/init.d/agent-adapter stop
/etc/init.d/agent-gw stop
/etc/init.d/agentd stop
apk del luci-app-agent-router nexus-agent-roles agent-adapter agent-gw agentd agent-netd
```

Inspect `/etc/config/agent*` and `/etc/agent-gw/` afterward. The package manager may retain configuration to prevent data loss. Delete it manually only after confirming that a backup exists and the identity, policy, and keys are no longer needed.

## Roll back

Reinstall the previous verified package batch as a unit and restore the backup. Do not roll back only `agentd` or `agent-gw`; their IPC and configuration contracts may no longer match.

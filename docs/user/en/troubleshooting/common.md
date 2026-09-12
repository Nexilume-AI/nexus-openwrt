---
sidebar_position: 1
title: Common problems
---

# Common problems

## Agent Router is missing from LuCI

Confirm that `luci-app-agent-router` is installed, then sign out and back in. If it remains missing, verify that the package matches the OpenWrt branch and target architecture.

## A service does not start

```bash
logread | grep -E 'agentd|agent-gw|agent-adapter'
uci show agent
```

Resolve configuration parse errors, occupied ports, invalid certificate paths, and missing dependencies first.

## An agent is running but not visible

Verify its router URL and credentials, host-to-router network path, DNS, and local exception output. If registration succeeds but no route appears, inspect the lease, capability name, and Policy RIB.

## A neighbor is visible but routes are not imported

Visibility does not imply trust. Check peer trust, import policy, and the peer's export policy. After revoking or rebuilding trust, wait for state to refresh.

## IPv6 ping works but calls fail

Check the listener address, TCP port, firewall, TLS hostname, and authentication in that order. A successful ping proves only that the ICMPv6 path works.

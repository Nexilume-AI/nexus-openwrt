---
sidebar_position: 3
title: Configure IPv6 access
---

# Configure IPv6 access

Distinguish host/VM management, private Agent callbacks and Internet-reachable Agent addresses. ULA is not public, and configuring DHCPv6 does not prove an upstream has assigned an address or prefix.

## Desktop VM: one-command setup

The launcher configures host `fd6e:6578:7573:246::2/64` and guest `fd6e:6578:7573:246::1/64`. Open [LuCI over IPv6](http://[fd6e:6578:7573:246::1]/). Set the root password first, then reapply from Administrator PowerShell:

```powershell
.\configure-ipv6.ps1
```

For upstream IPv6, explicitly select an existing Hyper-V upstream switch:

```powershell
Get-VMSwitch
.\configure-ipv6.ps1 -Mode Upstream -WanSwitchName 'Default Switch'
```

Replace the switch with the actual upstream network; NAT switches do not guarantee public IPv6. The script configures DHCPv4/DHCPv6 on a separate WAN and reports `wan6` status without advertising a public prefix on the management LAN. Empty address/prefix fields mean no assignment yet. Supply the same `-InstallationDirectory` for custom installations. This script targets launcher-managed Windows/Hyper-V instances, not production routers. See [desktop delivery status](../getting-started/desktop-vm.md).

## Router-managed public Agent addresses

Open **Status → Agent Routing → Developer mode → Advanced Settings → Public Agent IPv6**. Current source defaults are `public_ipv6_enabled=1` and `public_ipv6_mode=auto`; they cannot supply public addresses without a valid upstream.

| Source | Requirement |
| --- | --- |
| Automatic | Prefer a detected routed/PD prefix, otherwise check a global upstream on-link `/64` |
| routed-prefix | An actual `/48`–`/64` routed to OpenWrt |
| upstream-relay | A valid upstream on-link `/64` and interface, using NDP proxying for `/128` addresses |

Here **upstream-relay is an IPv6/NDP mode**, separate from Cloud Relay and Open Mesh application relays.

After an Agent requests `public_ipv6="auto"`, inspect its actual `/128`, lease and connection descriptor in Local Agents. Public invocation also needs the configured gateway listener, TLS/authentication and firewall rules. Verify an actual call from an external network; a visible address or successful ping is insufficient.

## Private callbacks and host addresses

Private IPv4/ULA callbacks do not require a public `/64`. Windows Host Alias uses a host-owned prefix and is a separate deployment path; see [Windows IPv6](https://nexilume-ai.github.io/nexus-docs/en/sdk/guides/windows-ipv6). Never use documentation prefix `2001:db8::/32` as a real allocation or expose LuCI, ubus or diagnostic interfaces to the Internet.

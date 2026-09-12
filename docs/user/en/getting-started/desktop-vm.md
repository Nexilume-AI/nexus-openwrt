---
sidebar_position: 2
title: Try full OpenWrt on a computer
description: Run OpenWrt, LuCI, and Nexus Agent Router in a dedicated Hyper-V VM.
---

# Try full OpenWrt on a computer

The desktop path runs a complete OpenWrt guest with LuCI, Agent Router, and the first-node wizard. For production devices, continue using [installation and builds](install.md) within the [support matrix](support-matrix.md).

:::info Delivery status
The launcher, image recipe, and offline validation tests are available. **A preinstalled desktop image is not published, and full guest boot acceptance remains pending.** This is not yet a downloadable one-command installer. Existing Hyper-V router acceptance does not validate this new desktop image.
:::

## Two deployment paths

| Path | Use | Distribution |
| --- | --- | --- |
| OpenWrt device | Continuous operation and real network/device integration | Device-matched firmware or feed packages |
| Desktop VM | Learning, demos, development, single-node invocation | Clean preinstalled VHDX, checksum manifest, launcher |

The first launcher targets Windows x86-64 with Hyper-V and its PowerShell module enabled. Initial creation requires administrator rights. Defaults are 1 GiB RAM and two vCPUs. Linux/macOS, other hypervisors, and hosts without Hyper-V do not yet have a one-command launcher.

## Start a prepared bundle

A maintainer builds and accepts the clean image using `deploy/desktop/BUILD.md` in the source. The bundle must contain `desktop-image.json`, its matching VHDX, `start-nexus-openwrt.ps1`, and README. Source scripts alone are insufficient to boot.

Check publisher provenance and checksums, then run in Administrator PowerShell from the extracted folder:

```powershell
.\start-nexus-openwrt.ps1 -Action Check
.\start-nexus-openwrt.ps1
```

After guest boot, open [LuCI](http://192.168.246.1/). Set a unique root password, then open **Status → Agent Routing → User mode** and [create the trust domain and first node](quick-setup.md). Device identities, keys, and realm state must be generated per installation rather than embedded shared credentials.

The release image is copied to a separate writable disk under `%LOCALAPPDATA%\Nexus\OpenWrtDesktop`. Choose a new location using `-InstallationDirectory` on first start and use that same path in later commands.

```powershell
.\start-nexus-openwrt.ps1 -Action Status
.\start-nexus-openwrt.ps1 -Action Stop
.\start-nexus-openwrt.ps1 -Action Start
```

Stop requests graceful shutdown and preserves data; it never silently forces power-off. Check verifies release bytes/profile, not business-service health.

## Host and guest networking

A dedicated Internal switch uses host `192.168.246.2/24` and guest `192.168.246.1/24`. By default, there is no WAN, physical bridge, host default gateway/DNS change, or guest DHCP/RA advertisement.

For a host Python Agent, follow the SDK guide and bind to an address reachable from the guest, such as `192.168.246.2`. A host `127.0.0.1` listener cannot receive guest callbacks. If a firewall rule is needed, restrict it to the chosen Agent port and guest source address.

Verify local registration/invocation first. Then follow the bundle README to explicitly add a NAT WAN for Cloud/Relay tests. Set a password first, identify the new adapter by MAC in LuCI, and configure DHCP-client and the WAN firewall zone. Outbound NAT connectivity does not validate inbound public IPv6 or real-LAN discovery.

## One-command IPv6 configuration

The launcher also configures IPv6 on the dedicated network: guest `fd6e:6578:7573:246::1/64`, host `fd6e:6578:7573:246::2/64`. Open [LuCI over IPv6](http://[fd6e:6578:7573:246::1]/). These are private ULA addresses, not public Internet addresses.

The bundle includes `configure-ipv6.ps1`. Set the root password in LuCI and keep the VM running, then reapply local IPv6 from Administrator PowerShell:

```powershell
.\configure-ipv6.ps1
```

The script connects through Windows OpenSSH. Verify the host fingerprint on first connection and enter the root password when prompted, or use existing SSH authentication. Passwords are not stored. If you selected a custom installation location, pass the same `-InstallationDirectory` here.

To request IPv6 from an upstream network, choose an existing Hyper-V upstream switch:

```powershell
Get-VMSwitch
.\configure-ipv6.ps1 -Mode Upstream -WanSwitchName 'Default Switch'
```

Replace the example switch with one providing your upstream network. The script adds a separate WAN adapter and configures DHCPv4, DHCPv6, and the WAN firewall zone. Omit `-WanSwitchName` if WAN already exists. If there is no unique unused guest interface, verify its MAC in LuCI before specifying `-WanDevice eth1`. The management LAN remains separate.

The script prints actual `wan6` status. Empty address or prefix fields mean the upstream has not supplied them; this is not public IPv6 success. NAT switches do not guarantee public IPv6. This command does not advertise a public prefix on the management LAN. Public Agent ingress still requires service configuration and an external connectivity test.

## Verification and troubleshooting

- Sign in to LuCI, load Agent Router, and finish initial setup.
- Verify the Python Agent capability in Capability Routes and a successful authenticated invocation.
- Confirm persistence across reboot, graceful Stop/Start, and unchanged host networking.
- **Missing image:** obtain an accepted bundle or have a maintainer follow BUILD.md; never copy a used router disk.
- **Access denied:** check Administrator PowerShell and Hyper-V permissions.
- **Subnet/name collision:** existing resources are not overwritten. Do not remove routes to bypass checks; this fixed profile supports one desktop instance.
- **Interrupted creation:** objects/state remain for diagnosis; incomplete VMs are not automatically started.

This path does not establish production throughput, public IPv6, hardware-driver, or multi-node acceptance.

## Open-source release scope

Release OpenWrt separately from Cloud Community. Export reviewed router components, build tools, and their docs rather than uploading mixed workspace disks, secrets, runtime logs, or enterprise documentation. Nexus-authored sources follow LICENSE/NOTICE; bundled OpenWrt and third-party software retain their licenses and corresponding-source obligations.

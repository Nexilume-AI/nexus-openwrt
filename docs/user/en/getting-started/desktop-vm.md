---
sidebar_position: 2
title: Try full OpenWrt on a computer
description: Run OpenWrt, LuCI, and Nexus Agent Router in a dedicated Hyper-V VM.
---

# Try full OpenWrt on a computer

The desktop path runs a complete OpenWrt guest with LuCI, Agent Router, and the first-node wizard. For production devices, continue using [installation and builds](install.md) within the [support matrix](support-matrix.md).

## Two deployment paths

| Path | Use | Prepare |
| --- | --- | --- |
| OpenWrt device | Continuous operation and real network/device integration | Device-matched firmware or feed packages |
| Desktop VM | Learning, demos, development, single-node invocation | Clean preinstalled VHDX, checksum manifest, launcher |

This guide uses Windows x86-64 with hardware virtualization, Hyper-V and its PowerShell management module enabled. Use Administrator PowerShell to create and manage the VM. Defaults are 1 GiB RAM and two vCPUs. This launcher is specific to Hyper-V.

## 1. Prepare the host and bundle

Check the tools in Administrator PowerShell:

```powershell
Get-Command Get-VM, Get-VMSwitch, ssh.exe
Get-VMSwitch
```

If Hyper-V commands are missing, enable Hyper-V and its management tools in Windows Features, restart when prompted, and check again. The IPv6 helper also requires the Windows OpenSSH client.

Extract a prepared bundle containing these files to a regular local directory:

```text
Desktop bundle/
  desktop-image.json
  <image matching the manifest>.vhdx
  start-nexus-openwrt.ps1
  configure-ipv6.ps1
  README.md
```

The manifest records the image filename, SHA-256 and network profile. **The source ZIP does not contain a VHDX.** If you only have source, follow `deploy/desktop/BUILD.md`, then use `deploy/desktop/prepare-bundle.py` to create the bundle. Do not reuse a running router disk or rename an arbitrary VHDX to match the manifest.

Reserve disk space for the original image and a separate runtime copy. Verify the bundle's source and publisher checksum; the script's own check only confirms consistency with the manifest.

## 2. Check and start

Check publisher provenance and checksums, then run in Administrator PowerShell from the extracted folder:

```powershell
.\start-nexus-openwrt.ps1 -Action Check
.\start-nexus-openwrt.ps1
```

After guest boot, open [LuCI](http://192.168.246.1/). Set a unique root password, then open **Status → Agent Routing → User mode** and [create the trust domain and first node](quick-setup.md). Device identities, keys, and realm state must be generated per installation rather than embedded shared credentials.

The first start creates a dedicated switch and VM. If LuCI stays unavailable, open the `Nexus-OpenWrt-Desktop` console in Hyper-V Manager to inspect guest startup. Keep the original image unchanged.

The release image is copied to a separate writable disk under `%LOCALAPPDATA%\Nexus\OpenWrtDesktop`. Choose a new location using `-InstallationDirectory` on first start and use that same path in later commands.

As an alternative first start, choose a new dedicated location and resources:

```powershell
.\start-nexus-openwrt.ps1 -InstallationDirectory 'D:\NexusDesktop' -MemoryMiB 2048 -Processors 2
```

For that installation, append the same `-InstallationDirectory 'D:\NexusDesktop'` to subsequent commands. Do not treat an existing installation as a new instance. The examples below use the default location.

## 3. Status, shutdown and restart

```powershell
.\start-nexus-openwrt.ps1 -Action Status
.\start-nexus-openwrt.ps1 -Action Stop
.\start-nexus-openwrt.ps1 -Action Start
```

Stop requests graceful shutdown and preserves data; it never silently forces power-off. Check verifies release bytes/profile, not business-service health.

## 4. Connect an Agent on the host

A dedicated Internal switch uses host `192.168.246.2/24` and guest `192.168.246.1/24`. By default, there is no WAN, physical bridge, host default gateway/DNS change, or guest DHCP/RA advertisement.

For a host Python Agent, follow the SDK guide and bind to an address reachable from the guest, such as `192.168.246.2`. A host `127.0.0.1` listener cannot receive guest callbacks. If a firewall rule is needed, restrict it to the chosen Agent port and guest source address.

Enable **Agent services** in LuCI User mode, then [publish and invoke an Agent](../guides/publish-api.md) on the host. Verify a lease in Local Agents, a capability in Capability Routes, and an actual invocation response. Empty lists on first boot are normal; VM Running status does not prove service readiness.

The isolated network has no Internet access by default. For Cloud/Relay or package downloads, set a root password first, then select an upstream switch as described below while retaining the dedicated management LAN.

## 5. Local IPv6 and upstream networking

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

## 6. Connectivity checks and troubleshooting

- Sign in to LuCI, load Agent Router, and finish initial setup.
- Verify the Python Agent capability in Capability Routes and a successful authenticated invocation.
- Confirm persistence across reboot, graceful Stop/Start, and unchanged host networking.
- **Missing manifest/image:** check the bundle file list above. Source requires image building and packaging first; Check does not download or generate an image.
- **Access denied:** check Administrator PowerShell and Hyper-V permissions.
- **Subnet/name collision:** existing resources are not overwritten. Do not remove routes to bypass checks; this fixed profile supports one desktop instance.
- **Interrupted creation:** objects/state remain for diagnosis; incomplete VMs are not automatically started.

Check management connectivity from the host:

```powershell
Test-NetConnection 192.168.246.1 -Port 80
Test-NetConnection 192.168.246.1 -Port 22
Test-NetConnection 'fd6e:6578:7573:246::1' -Port 80
```

Reachable management ports do not verify Agent calls; check registration, authentication and callback reachability separately. Local ULA management can work even when the upstream supplies no IPv6 address.

## 7. Keep data or remove the VM

Stop preserves the runtime disk and Start reuses the configuration. Before migration or reinstallation, shut down normally and back up the runtime disk and `desktop-state.json` from the installation directory.

For removal, identify `Nexus-OpenWrt-Desktop` in Hyper-V Manager and remove that VM and its dedicated Internal switch. Delete its installation directory only after deciding the data is no longer needed. Leave other VMs, switches and host routes intact.

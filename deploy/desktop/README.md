# Nexus OpenWrt Desktop VM

Windows/Hyper-V desktop launch kit for the full OpenWrt + LuCI + Nexus Agent
Router stack. The production device/feed installation remains separate.

The source checkout contains the launcher and clean-image recipe. A prepared
bundle contains a preinstalled VHDX and needs no host SDK or compilation.
The 2026-09-13 candidate passed real Hyper-V first-boot and restart acceptance;
see [the acceptance record](ACCEPTANCE.md) for its exact image hash and scope.
GitHub publication of the binary is separate from this local validation.
Never redistribute a disk copied from an existing or Cloud-connected VM.

## Run a prepared release bundle

Prerequisites: x86-64 Windows with Hyper-V and its PowerShell module enabled,
hardware virtualization, and an Administrator PowerShell. Other hypervisors,
Windows installations without Hyper-V, Linux and macOS are not yet covered by
this launcher. No host Python, SDK, Docker, or OpenWrt compilation is needed by
the end user of a prepared bundle.

The bundle contains `desktop-image.json`, its VHDX, this guide, and the launcher.
Verify its publisher/release checksum before extraction. In the extracted folder:

```powershell
.\start-nexus-openwrt.ps1 -Action Check
.\start-nexus-openwrt.ps1
```

Open [LuCI](http://192.168.246.1/) after the guest boots. Set a unique root
password first, then open **Status → Agent Routing → User mode** and use the existing
first-node/trust-domain setup. Enable **Agent services** for the local SDK listener
at `http://192.168.246.1:7446` using `auth="auto"`. The fresh desktop configures
JWT verification and source-bound LAN sessions, with public ingress disabled.
A unique verifier public key is generated on first boot; its signing private
key is discarded. Cloud enrollment may replace the issuer configuration later.
Device identities and signing keys must be
generated in this installation, never shipped in the image.

The r5 image sets system UTC from the Hyper-V hardware clock during first setup
and at every boot, including offline use. Keep the Windows host clock correct.
It does not read time from an unauthenticated network endpoint. If the Hyper-V
clock is absent or invalid, inspect `logread -e nexus-desktop-time` and correct
time before pairing. Older r4 images require manual clock synchronization.

The script copies the release disk to `%LOCALAPPDATA%\Nexus\OpenWrtDesktop`,
creates a Generation 2 VM with 1 GiB RAM and two CPUs, and disables Secure Boot
for this image profile. Use `-InstallationDirectory`, `-MemoryMiB` and `-Processors`
on first start to select another location/size. Use the same installation path
on later commands. Only one instance of this fixed network profile is supported.

```powershell
.\start-nexus-openwrt.ps1 -Action Status
.\start-nexus-openwrt.ps1 -Action Stop
.\start-nexus-openwrt.ps1 -Action Start
```

Stop requests graceful guest shutdown and preserves the disk. It does not force
power-off. A running VM is not proof of Agent service readiness. Check verifies
release bytes/profile, not the mutable installed disk, boot health, or provenance.
`-WhatIf` performs preflight but does not create or start the VM.

## Network and first Agent

For host Python Agents, the follow-up acceptance bundle includes SDK 0.46.2.
Install its wheel in your chosen Python environment before testing Cloud
re-registration (Python is needed for a host Agent, not for VM startup):

```powershell
python -m pip install .\nexus_agent_sdk-0.46.2-py3-none-any.whl
```

This SDK waits through the temporary Cloud `unavailable` state while node
presence recovers. Allow up to 300 seconds in `wait_for_cloud(timeout=300)`;
the connector normally reconciles every 120 seconds. This local wheel has not
been published to PyPI by this acceptance task.

The dedicated **Internal** switch connects only this VM and the host. The host
uses `192.168.246.2/24`, the guest `192.168.246.1/24`. No default gateway, DNS,
host firewall changes, physical bridge, WAN, or DHCP advertisements are added.
Existing overlapping routes, VM names, switches and installation directories
cause a refusal rather than being reused or replaced.

Run the Python Agent on the host with an explicit listener reachable at
`192.168.246.2`, following the SDK first-Agent guide and the router's current
authorization flow. If Windows blocks guest-to-host callbacks, allow only the
chosen Agent port from `192.168.246.1` according to your host policy. Confirm that
the capability appears in LuCI and that an authenticated invocation returns the
expected result. Loopback-only host listeners are not reachable from the VM.

LAN broadcast discovery, inbound public IPv6 and real-device throughput are not
validated by this isolated desktop network. Production support remains bounded
by the device/firmware acceptance matrix.

## One-command IPv6 configuration

The initial launcher already configures private ULA addresses: guest
`fd6e:6578:7573:246::1/64`, host `fd6e:6578:7573:246::2/64`.
Open `http://[fd6e:6578:7573:246::1]/` for IPv6 management. ULA is local/private,
not Internet-routable. To reapply it, use Administrator PowerShell:

```powershell
.\configure-ipv6.ps1
```

The script uses OpenSSH to configure the guest and prompts for the root password
or uses your existing SSH authentication. It does not store passwords and checks
the guest host key. Set the root password in LuCI before using it. For existing
key-based access, use `-IdentityFile` and `-KnownHostsFile`; the latter requires
a previously verified host-key file and enforces strict verification.

To add a WAN on an existing upstream switch and request DHCPv6 addresses/prefixes:

```powershell
.\configure-ipv6.ps1 -Mode Upstream -WanSwitchName 'Default Switch'
```

Choose an existing switch that actually provides the desired upstream. A NAT
switch may have no public IPv6. If WAN already exists, omit `-WanSwitchName`.
The script detects a single unused guest interface or accepts an explicit
`-WanDevice eth1` after you verify its MAC in LuCI. It refuses to adopt a LAN
bridge/member, move an existing WAN, or invent an Internet address.

Upstream mode configures DHCPv6 and requests a delegated prefix, then reports
`wan6` status. Empty address/prefix fields mean the upstream has not supplied
them. It does not advertise a delegated public prefix on the isolated LAN or
claim remote reachability; enabling public Agent ingress remains a separate
authorized Router configuration and external test.

## Manual WAN alternative

To test Cloud/Relay or install additional packages, explicitly add a WAN adapter
to an existing NAT-capable Hyper-V switch, for example `Default Switch` when it
is present. Inspect switch names first; never replace the LAN switch with a
physical bridge just to obtain Internet access.

```powershell
Get-VMSwitch
Add-VMNetworkAdapter -VMName Nexus-OpenWrt-Desktop -Name WAN -SwitchName 'Default Switch'
```

In LuCI, identify the **new** adapter by its MAC address (do not assume it is
always eth1), create a DHCP-client WAN interface, and assign it to the standard
WAN firewall zone. Keep management restricted to LAN. A NAT WAN can provide
outbound connectivity; it does not establish inbound IPv6 reachability. Trust
domain, Relay and Directory setup remains explicit in Agent Router.

## Troubleshooting and removal

- Missing manifest/image: this is a source checkout, not a prepared release.
- Access denied: use Administrator PowerShell and verify Hyper-V access.
- Subnet conflict: do not delete existing routes to make the launcher pass.
  This fixed profile needs a non-overlapping host; an alternate subnet requires
  a matching image and launcher profile.
- Startup failed halfway: objects and state are retained for diagnosis. An
  incompletely configured VM will not be silently started by the launcher.
- No LuCI: inspect the VM console and guest LAN address in Hyper-V Manager.
  Check DHCP is disabled and the host dedicated adapter is `192.168.246.2/24`.
- Cloud authentication or publication stalls: before pairing, check the guest
  clock in LuCI System settings and synchronize it with the browser or a working
  NTP server. A fresh Hyper-V guest may interpret the host local RTC as UTC.
  If correcting a large clock offset after pairing, restart `nexus-cloud` and
  the test Agent, then allow one reconciliation interval (up to 120 seconds).
- Shutdown unavailable: use the guest console to shut down; the launcher does
  not turn a failed graceful shutdown into a forced power-off.

For removal, shut down, back up the installation disk if needed, then remove only
the recorded VM and dedicated internal switch in Hyper-V Manager. Delete the
installation directory only after confirming its absolute path and contents.
No automatic recursive-delete command is supplied.

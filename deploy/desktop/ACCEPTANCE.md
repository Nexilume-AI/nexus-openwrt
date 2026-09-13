# Desktop image acceptance — 2026-09-13

Candidate: OpenWrt 25.12.4 x86/64 EFI, Hyper-V Generation 2, 1 GiB RAM,
2 vCPUs, 512 MiB root filesystem. Local acceptance; no public binary release
or upstream/public IPv6 acceptance is implied.

Unbooted VHDX SHA-256:
`9086547371063b83bebe30f20cfa56a8bcdd6785152e393a12a26290627d50ea`

## Passed on a fresh copy

- Launcher checksum preflight, creation and first boot. The release VHDX was
  never booted; only its separately installed copy was used for acceptance.
- Empty initial root password; setting a private root password and subsequent
  authenticated LuCI login. Agent Routing User mode loads.
- Enabling Agent services through the authenticated LuCI RPC API on the very
  first boot, without service patches or an intervening reboot.
- Host Python SDK Agent registration, lease renewal and actual Echo invocation
  through `http://192.168.246.1:7446` with automatic LAN-session authentication.
  Missing and invalid tokens are rejected. Test Agent unregisters on close.
- IPv4 and ULA IPv6 HTTP management. Two consecutive executions of
  `configure-ipv6.ps1 -Mode Local` succeed using verified SSH key access.
- Normal launcher Stop/Start after the OS has completed boot. A changed kernel
  boot ID confirms the restart; root login, enabled services, verifier key and
  device TLS identity persist. SDK registration, renewal and invocation pass again.
- Dedicated Internal switch with one management NIC, DHCP/RA/DHCPv6 serving
  disabled, no WAN or physical bridge. Host IPv4/IPv6 default routes unchanged.
  The temporary host callback firewall rule was removed after acceptance.
- Generated rootfs scan: no preset root password hash, private key material,
  authorized test SSH keys or Cloud enrollment identity. First-boot identity
  generation remains in the image.
- Nine desktop bundle regression tests pass, including checksum/profile/path
  refusal and Hyper-V child-directory identity handling for lifecycle and IPv6.

Key installed packages: `agentd 3.1.0-r26`, `luci-app-agent-router 3.1.0-r22`,
`agent-gw 0.22.0-r26`, `nexus-node-runtime 20.20.2-r1`,
`nexus-agent-router-seed 1.0.0-r2`, Relay and Directory `1.2.0-r2`.
The prepared bundle includes the complete firmware and custom-package manifests.

## Boundaries

This acceptance covers isolated Windows/Hyper-V desktop use, not upstream
DHCPv6 prefix delegation, inbound public IPv6, Cloud enrollment, external Relay
connectivity, other hypervisors or production throughput. Shutdown was tested
after boot completed; wait for LuCI before issuing lifecycle commands.

The firmware contains separately licensed upstream OpenWrt packages. Preserve
its package/license inventory and corresponding source obligations when publishing
binaries; the Nexus source license does not relicense the complete firmware.

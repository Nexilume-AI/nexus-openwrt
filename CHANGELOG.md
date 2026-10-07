# Changelog

## v0.1.0-beta.1 — 2026-10-07

- Signed APK feed for OpenWrt 25.12.4 x86/64 with Router, Relay and Seed installation profiles.
- Explicit package inventory, SDK/source checksums, signing-key verification and installation instructions.
- Router profile remains independent of Node.js; optional Relay/Seed profiles include the Node runtime.
- Trusted-LAN registration before Cloud enrollment, Mesh setup/recovery and MCP compatibility fixes.
- Fix LuCI health checks rejecting healthy services with OpenWrt's synchronous curl resolver.

This is a Beta package release, not a firmware or preconfigured VM image.
See the GitHub release notes for target acceptance results and limitations.

## Initial standalone source candidate

- OpenWrt feed for Agent routing, gateway, adapter, discovery and LuCI management.
- Nexus Cloud client connectivity via Direct IPv6 or Relay.
- Optional self-hosted Open Mesh Relay and Directory packages.
- Hyper-V desktop launch/bundle scripts and IPv6 configuration workflow.
- Chinese and English OpenWrt user documentation.
- Independent host CI and source-only export boundaries.

Desktop preinstalled images remain a separate release gate. Package versions
are maintained in individual feed Makefiles.

# Desktop time and cold-start acceptance — 2026-09-13

The r4 guest reproduced a large UTC offset after normal Hyper-V Stop/Start,
despite the host Time Synchronization integration service being enabled.
The guest exposes a `hyperv` PTP clock with correct UTC. r5 includes upstream
`linuxptp 4.4-r1` and a desktop-only early boot service that samples that clock
and sets CLOCK_REALTIME before desktop identity setup. Later NTP behavior is
unchanged. The host remains the trusted source for offline boot time.

Unbooted r5 VHDX SHA-256:
`17045f3810277d413ee160c3dff3bdff093d3b545122db15e141a707831d39a8`

## Passed on a fresh installed copy

- Initial boot with only the isolated management LAN and no WAN; UTC agrees
  with the host without manual time setting or Internet NTP.
- Password setup, authenticated LuCI, first-time Agent enablement, health
  endpoint and private IPv6 management.
- Normal launcher Stop/Start while still offline: changed boot ID, unchanged
  verifier/TLS identity hashes, UTC within 3 seconds of the host, and services
  ready after restart. The installed service matches the reviewed source.
- Clock service rejects missing/ambiguous Hyper-V clocks and malformed or
  multiple time samples without setting the system clock. Valid samples set
  only CLOCK_REALTIME. Three Linux regression tests cover these cases.
- Clean rootfs scan found no preinstalled private keys or Cloud enrollment.

The r5 image does not need the r4 manual time-calibration workaround when a valid
Hyper-V clock is present. Missing/invalid clock sources still require operator
diagnosis; this is not a fallback to untrusted network time. Public IPv6 ingress,
other hypervisors and production throughput remain outside this acceptance.
The SDK 0.46.2 wheel is included for temporary Cloud publication-state recovery.
See CLOUD-ACCEPTANCE.md for the earlier Relay and same-origin reuse tests.

## r6 Cloud jail startup fix

The paired r5 restart exposed a separate startup-order defect: agentd mounted
the Cloud tenant projection directory only if it already existed. On cold boot
the connector had not created it yet. The tunnel and route could recover while
real calls failed with `relay_reset`, because the jail could not read the tenant
projection. The missing file inside the agentd jail was confirmed directly.

agentd `3.1.0-r27` creates the root-owned directory with mode 0755 before opening
its procd instance and always mounts it read-only. Six tenant-alias contract
checks pass; the complete private CI also passes. r6 includes this fix and the
same offline clock service, plus SDK 0.46.2.

Unbooted r6 VHDX SHA-256:
`6742f6391a0277f4e8b4c5ec4ee85ca99bbc1c5731621a8b30f79304ccc3a844`

The clean r6 installed copy passed first boot, automatic UTC, authenticated LuCI,
Agent enablement, Cloud pairing and real MCP Echo invocation. It was then stopped
and started through the normal launcher while the host Agent process stayed up.
Without another pairing, manual clock setting or Agent restart, the Relay tunnel,
SDK route and Cloud registration recovered using their normal lease/sync periods.
The Cloud identity hashes survived, UTC was within 4 seconds of the host, and
the tenant projection was readable inside the cold-start agentd jail. The final
MCP initialize, tools/list and tools/call all returned HTTP 200; Echo returned
the original payload with `isError: false`.

Recovery is asynchronous and may take several minutes with default leases; this
does not promise uninterrupted calls during a VM reboot. The end-to-end test
uses the separate local Community environment described in CLOUD-ACCEPTANCE.md.
No public binary publication or public IPv6 inbound acceptance is implied.

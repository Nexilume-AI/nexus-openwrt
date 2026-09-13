# Desktop r5 time and restart acceptance — 2026-09-13

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

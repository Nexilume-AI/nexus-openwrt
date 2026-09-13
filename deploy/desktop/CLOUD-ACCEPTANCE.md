# Desktop Cloud / Relay acceptance — 2026-09-13

Test environment: a real Hyper-V OpenWrt 25.12.4 VM and a separate local
Nexus Cloud Community Compose project with its own database, keys and Relay.
The HTTPS test gateway verifies certificates and forwards the verified device
identity; it is acceptance infrastructure, not the production gateway installer.

## Passed

- Pairing through the authenticated LuCI RPC API, device certificate issuance,
  and all 12 connector TLS/JWT diagnostics.
- Native Directory assignment and lease renewal with certificate verification.
- Outbound Relay TLS/HTTP2 session and invocation tunnel establishment.
- Host Python SDK Agent registration without a public IPv6 allocation, Cloud
  publication using Relay, MCP initialization and tool discovery.
- A real `tools/call` traverses Cloud → Relay → OpenWrt → host Echo Agent and
  returns the original JSON payload with `isError: false`.
- Restarting the isolated Relay restores the tunnel automatically; the same
  MCP call succeeds again.
- The upstream IPv6 script attaches the VM to an existing external switch.
  The guest obtains an IPv6 address and default route; IPv6 HTTPS to Baidu
  returns HTTP 200. IPv4 HTTPS also succeeds.

## Fixes required by this acceptance

- Connector r43 derives the default Open Mesh Router ID from the same interface
  MAC as agentd. Enrolling the literal `router-local` placeholder caused
  Directory identity rejection.
- Community device endpoints preserve the standard Edge response envelope.
- Community Relay provisioning supplies `connect_ipv4` to the native Directory
  contract, including compatibility with existing installation receipts.
- Nineteen Community Edge/certificate/Relay tests pass; the connector contract
  check passes.

The initial integration run used the r3 VM with the connector source fix.
The rebuilt r4 image then passed fresh boot, authenticated LuCI, first-time
service enablement, local IPv6 management, Cloud pairing, Relay establishment,
and the full MCP Echo invocation without runtime code patches. Connector r43
is installed in this image. Community fixes were mounted into the isolated
test service. The earlier r3 ZIP does **not** contain connector r43.

r4 unbooted VHDX SHA-256:
`b718b72cd8bf4ac4f9ece855102b0a9b4202d1006fdc7f0a8f20000fdd482213`

Fresh Hyper-V RTC time was eight hours ahead of UTC in this environment. The
guest clock was synchronized to the host UTC time and the connector restarted
before the final call. Check time in LuCI before Cloud pairing, as described
in README. This is a required configuration precondition, not an automatically
validated time-sync feature of the launcher.

## Scope limits

The upstream supplied no delegated IPv6 prefix. Public IPv6 ingress, downstream
prefix delegation, production throughput and other hypervisors remain untested.
One IPv6 HTTPS target timed out while another succeeded; this is not a claim
that every Internet destination is reachable. MCP acceptance used JSON requests;
streaming is not supported by this runtime. No public binary was published.

## Same-origin re-registration follow-up

The earlier SDK error was reproduced: the Cloud registration API legitimately
returned `unavailable` while node presence recovered, but SDK 0.46.1 rejected
that state. SDK 0.46.2 accepts it as non-ready and continues bounded polling.
The regression failed before the fix; all 24 facade tests pass afterward.

Using the old `agent://desktop-test/relay-echo` origin on the replacement VM
now reaches `ready` and completes a real MCP Echo call. After stopping the Agent,
reconciling its withdrawal and starting it again, both the Cloud Agent ID and
Runtime ID are unchanged and the Echo call succeeds again. The built wheel was
installed locally, its patched modules matched the tested source, and its
non-ready state handling was checked in an isolated Python process.

This verifies same-origin re-registration and stable identity across Agent
stop/start on the replacement VM. It does not claim to transfer the original
VM's Cloud Agent ID to another router or override ownership boundaries.
The follow-up bundle adds SDK 0.46.2; the r4 VHDX bytes remain unchanged.

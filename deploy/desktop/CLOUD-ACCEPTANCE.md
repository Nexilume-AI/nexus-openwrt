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

The successful integration run used the r3 VM with the connector source fix
installed for validation and the Community fixes mounted into the test service.
The earlier r3 ZIP does **not** contain connector r43. A rebuilt image needs its
own fresh-boot acceptance before it can inherit this result.

## Scope limits

The upstream supplied no delegated IPv6 prefix. Public IPv6 ingress, downstream
prefix delegation, production throughput and other hypervisors remain untested.
One IPv6 HTTPS target timed out while another succeeded; this is not a claim
that every Internet destination is reachable. MCP acceptance used JSON requests;
streaming is not supported by this runtime. No public binary was published.

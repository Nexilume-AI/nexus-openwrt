# Generic outbound model Relay

SDK 0.53.0 adds a generic forwarding worker to the existing device login and
model registration flow. No `nexus_cli`, inbound device port, `key-file` or
`state-file` is required. Install repository source on Linux/OpenWrt Python 3.11+
until this version is published to PyPI:

```bash
python -m pip install 'nexilume[relay] @ git+https://github.com/Nexilume-AI/nexus-agent-sdk-python.git@main'
```

## Adopt an existing Provider

Create a manifest containing every existing Source model for the Provider.
It contains identifiers, not credentials:

```json
[
  {
    "name": "My model",
    "model": "exact-upstream-model-id",
    "endpoint": "https://api.example.com/v1",
    "protocol": "openai-compatible",
    "transport": "outbound",
    "provider_id": "EXISTING-PROVIDER-UUID"
  }
]
```

```bash
export NEXUS_RELAY_DEVICE='My host model relay'
nexus-relay login --models-json models.json --no-wait
# Open the printed URL and approve the device in the correct Organization.
nexus-relay login --resume
nexus-relay model register exact-upstream-model-id
nexus-relay serve
# In another terminal with the same device identity:
nexus-relay model activate exact-upstream-model-id
```

Use separate device identities for separate Organizations. `register` prepares
the tunnel without changing the Provider URL. `activate` verifies the catalog
through the live tunnel, then switches the Provider and its Sources together.
All approved models must be advertised. One activation covers the entire
Provider binding. Concurrent edits stop activation instead of being overwritten.
Existing Provider, Source and Router IDs, keys and Router bindings stay intact.
Keep the old transport until Router JSON and streaming calls pass.

To create a new Provider, omit `provider_id` and import its upstream key once
with `model register MODEL --prompt-endpoint-key`. Activation uses the standard
Source/Pool/Router registration flow. Existing Provider adoption instead reuses
Cloud's encrypted credential and rejects key replacement.

The Python API adds `RelayModelClient.transport_status()` and
`RelayModelClient.activate_model(model)`. Run forwarding with
`await nexus_agent.model_relay.serve(client)`.

## Lifecycle and failure behavior

Run `nexus-relay serve --device 'My host model relay'` as an unprivileged service
with `Restart=on-failure` and a private writable home. Credentials remain in the
existing SDK private store. The worker refreshes credentials and reconnects
with bounded backoff. `nexus-relay refresh` rotates credentials explicitly;
`nexus-relay revoke --yes` blocks new requests immediately and closes active
transports on the next check (normally within 22 seconds).

Revocation does not delete adopted resources or switch them to direct access.
Rebinding requires an operator to review and detach the revoked binding first.

Interrupted generations are never automatically replayed. An interrupted stream
remains incomplete, rather than being reported as successful. There is no fixed
total stream duration; idle upstream/backpressure timeout is 180 seconds.
Each device allows eight concurrent requests, returning 429 when saturated.
Requests are limited to 16 MiB; chunk queues are bounded.

Both ends enforce approved upstream, Provider, protocol and model IDs. HTTPS
verification stays enabled; redirects and private/reserved upstream addresses
are rejected. Supported public HTTPS routes are OpenAI-compatible `/models`,
`/model/info`, `/chat/completions`, `/responses` and System One `/systemone`.
This is not an arbitrary HTTP proxy. The Provider key is transient request
data, never returned in status or logged. Router IAM, usage, audit and model
capability metadata remain in Cloud.

## Cloud operator deployment

Deploy the matching Cloud source, apply account migrations, and restart Web.
Install this SDK with `[relay]` in a dedicated environment and run its generic
broker as an unprivileged loopback-only service:

```bash
python -m nexus_agent.model_relay \
  --control http://127.0.0.1:8002/api/v1/auth/relay/ \
  --host-header cloud.example.com --port 18764
```

Proxy `/api/model-relay/` to `http://127.0.0.1:18764/` through the existing TLS
ingress. Enable HTTP/1.1 WebSocket upgrade, disable buffering, allow 16 MiB
requests, and use idle proxy timeouts above 180 seconds. Keep the broker port
private. Cloud rechecks the device owner, Organization permissions, expiry,
revocation and Provider state. No provider-specific proxy or shared worker
secret is required.

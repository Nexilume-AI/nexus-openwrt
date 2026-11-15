# Relay model login and registration

Use the OpenWrt **`nexilume` Python SDK**, independently of `nexus_cli`.
The initial implementation manages an existing authenticated HTTPS model
endpoint. It does not open an inbound port or create an inference server.
An existing outbound Relay transport can continue serving that endpoint.

## SDK command line (0.52.1+)

### Install from PyPI

SDK 0.52.1 is available on PyPI. In a Linux/OpenWrt virtual environment:

```bash
python -m pip install -U nexilume
nexus-relay --version
nexus-relay --help
```

The Relay command is included in the dependency-free core; no extra or separate
CLI package is needed. Use `--index-url https://pypi.org/simple` if a mirror has
not synchronized yet. The core wheel supports Python 3.9+; building from source
requires Python 3.10+. Credentials remain in the same private device store when
the package is upgraded.

### Install from source

Use Linux/OpenWrt with Python 3.9 or newer and a writable private home directory.
The commands below install the current repository version, including changes
that may not yet be published to PyPI:

```bash
python3 -m venv ~/.local/share/nexus-relay-venv
~/.local/share/nexus-relay-venv/bin/python -m pip install \
  'git+https://github.com/Nexilume-AI/nexus-agent-sdk-python.git@main'
export PATH="$HOME/.local/share/nexus-relay-venv/bin:$PATH"
nexus-relay --version
```

For reproducible installations, replace `main` with a reviewed commit SHA.
On OpenWrt builds without venv/pip, install the SDK wheel using the image's
Python package tooling, or run it from a source checkout with `PYTHONPATH=src`
and `python3 -m nexus_agent.relay_cli`. Do not install `nexus_cli`.

Installing the SDK also installs **`nexus-relay`**. It uses Python's standard
library and the same `RelayModelClient`, with no `nexus_cli` dependency. An
equivalent module entry is `python -m nexus_agent.relay_cli`.

```bash
# Keep the device name stable; it identifies the SDK's private credential store.
export NEXUS_RELAY_CLOUD=https://cloud.nexilume.com
export NEXUS_RELAY_DEVICE=my-model-host

nexus-relay login --model my-model-v1 \
  --endpoint https://models.example.com/v1 \
  --protocol openai-compatible --name "My model"
# Open the printed URL and confirm the device + Organization in your browser.

# Existing endpoint: import its key once using a hidden prompt.
nexus-relay model register my-model-v1 --prompt-endpoint-key

# Subsequent registrations use stored credentials and preserve resource IDs.
nexus-relay model register my-model-v1
nexus-relay status
nexus-relay refresh
```

Use `--cloud` and `--device` instead of environment variables if preferred;
these options also work after the subcommand. Cloud defaults to
`https://cloud.nexilume.com`; the device name defaults to the hostname. Use the
same user, Cloud origin and device name as the Python API to reuse its binding.

For headless or deferred confirmation:

```bash
nexus-relay login --model my-model-v1 --endpoint https://models.example.com/v1 --no-wait
nexus-relay login --resume
```

`--no-wait` prints the verification URL and short code, never the device secret.
`--resume` continues the saved pending login without generating another code.
Login waits up to 600 seconds by default (`--wait-timeout`); Ctrl+C preserves the
pending request. The Cloud code still expires after ten minutes.

For multiple models, use `login --models-json models.json`, containing an array
of up to 32 distinct declarations with exactly `name`, `model`, `endpoint`, and
`protocol`. This is a declaration file, not a key or state file. Don't put secrets
in it. Changes to the approved scope require a new browser-confirmed binding.

Automation can pipe an existing endpoint key from its secret manager to
`model register MODEL --endpoint-key-stdin`. There is deliberately no raw `--key`
argument. Without either import option, registration uses the SDK's saved key;
it does not invent or change the endpoint's authentication configuration.

`--json` emits machine-readable output; waiting instructions use stderr.
Exit codes: **0** success/cancelled/explicit no-wait, **1** SDK or Cloud error,
**2** invalid arguments/storage, **3** waiting deadline reached, **130** interrupted.
Use the returned `status` to distinguish pending/cancelled states from connected.

```bash
nexus-relay revoke          # interactive confirmation; disables this device's Sources
nexus-relay revoke --yes    # explicit confirmation for unattended scripts
```

The CLI shares the Linux/POSIX credential-store requirement with the SDK.

## Bind a Linux/OpenWrt device

```python
import time
from nexus_agent import RelayModelClient

relay = RelayModelClient("https://cloud.nexilume.com", "my-model-host")
models = [{
    "name": "My model",
    "model": "my-model-v1",
    "endpoint": "https://models.example.com/v1",
    "protocol": "openai-compatible",  # or system_one
}]
login = relay.begin_login(models)
print(login["verification_uri_complete"], login["user_code"])
while not relay.poll_login():
    time.sleep(5)
```

The human signs in to Cloud, verifies the device code, reviews the exact models
and endpoints, chooses their Organization and confirms. Codes last ten minutes
and can only be redeemed once. The short code is not an API credential.

The device is authorized only for the immutable manifest it requested (up to
32 distinct model IDs). Adding an endpoint/model requires a new confirmed grant.
Current Organization membership and Provider/Source/Router administration
permissions are checked again on each device request. These credentials cannot
access general Cloud management APIs or inference endpoints.

## Register a model

Configure your endpoint's authentication middleware with the SDK-generated
secret, without displaying or logging it:

```python
endpoint_secret = relay.endpoint_api_key("my-model-v1")
# Install endpoint_secret in your endpoint's own server-side auth middleware.
receipt = relay.register_model("my-model-v1")
print(receipt["router_id"])
```

For an existing authenticated endpoint, migration code may instead call
`register_model(model, endpoint_api_key=existing_secret)` once, obtaining the
secret from that service's existing secret manager. It is automatically saved
and subsequent calls need only `register_model(model)`. No `key-file` or
`state-file` parameters are needed. The SDK does not change another application's
authentication middleware implicitly.

Cloud discovers the approved model in the endpoint's real catalog, checks health
and creates a private Provider → Source → Model Pool → execution Router. It
does not invent a missing model or restore an administrator-disabled resource.
Repeating registration is idempotent. Cloud holds the canonical registration
record; the SDK saves its receipt automatically. A lost response can be reconciled
using `relay.status()` or repeating the same registration.

Normal callers obtain their **Router API Key** in Cloud and use the Router's
exported API. Device access tokens are not inference keys.

## Storage, refresh and revoke

- Default private storage: `$XDG_DATA_HOME/nexilume/relay/<identity>/device.json`,
  or `~/.local/share/nexilume/relay/<identity>/device.json`.
- Identity includes Cloud origin and device name. Directory `0700`, files `0600`,
  exclusive OS file lock, atomic replacement and fsync. Symlink files and unsafe
  permissions are rejected. The credential store currently targets Linux/POSIX.
- Access tokens last ten minutes and refresh automatically before SDK requests.
  Refresh tokens rotate on every renewal, remain valid for seven days, and cannot
  extend the thirty-day browser-approved device grant. Keep a service calling
  `status()` periodically for an unattended host. A host offline beyond the
  refresh window needs a new browser confirmation.
- `relay.refresh()` explicitly rotates credentials; it returns no secrets.
- `relay.revoke()` invalidates both credentials, disables only that device's
  registered Sources/Providers, and clears local tokens. Other devices and
  existing non-SDK registrations are unaffected.
- Browser revocation is available at
  `/api/v1/auth/relay/page/` under **Your Relay devices**, including when the
  original host is lost. Calling `begin_login` again replaces local credentials
  only after Cloud rejects them, never silently replacing a working grant.
- Refresh/exchange responses lost after successful rotation fail closed:
  begin a new device login rather than replaying an old refresh credential.
- Revocation blocks new Router invocations; it does not replay/cancel inference
  already dispatched to an independent backend or rotate that backend's own key.

Keep this directory out of source control, logs, support bundles and backups
without encryption. The Cloud database retains only hashes of device tokens;
the endpoint key uses the existing encrypted Provider credential storage.

## Troubleshooting

| Result | Action |
| --- | --- |
| Expired code or denied login | Run `login` again and confirm the new code in the browser. |
| No saved binding on another shell | Use the same OS user, Cloud origin, device name and `XDG_DATA_HOME`. |
| Missing endpoint key | Import the endpoint's existing credential once with `--prompt-endpoint-key`. |
| Model missing/unhealthy | Check the endpoint catalog and authentication; registration does not force a healthy status. |
| Membership changed or grant expired/revoked | Obtain a new explicit browser approval; retries do not restore permission. |
| Connection lost during registration | Check `status`, then repeat the same registration to reconcile its resource IDs. |
| Connection lost during token rotation | Start a new login if the saved credential was already consumed. |

Auto-refresh happens on SDK requests, not in a daemon installed by the CLI.
For an always-on device, schedule `nexus-relay status` periodically under the
same OS user (for example every four minutes); stop and request new approval on
authentication failure. Never log the private store or endpoint credentials.

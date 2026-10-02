# Nexus OpenWrt

**English** · [Chinese](README_GUIDE_zh.md)

Agent registration, discovery, capability routing and invocation gateways on OpenWrt, with LuCI management, a Cloud connector and optional self-hosted Relay / Directory services.

## Release scope

This is an independent source-release candidate for OpenWrt 25.12.x. The current image-build baseline is 25.12.4 x86/64. Other architectures require a matching SDK and device validation. This repository does not include Nexus Cloud Server, TokenBank or the separately released Python SDK.

Desktop Hyper-V launchers and IPv6 configuration scripts are provided, but **preinstalled images and real boot acceptance are not yet complete**. Passing source tests does not certify every router model or desktop image for production. Never attach internal disks, live configuration or signing private keys to a release.

## Where to start

- [English manual](docs/user/en/index.md) / [Chinese manual](docs/user/zh/index.md)
- [Choose an installation path](docs/user/en/getting-started/choose-path.md) / [Two-router tutorial](docs/user/en/tutorials/two-router.md)
- [Cloud Relay](docs/user/en/guides/cloud-relay.md) / [Self-hosted router roles](docs/user/en/guides/router-roles.md)
- [Desktop VM startup and IPv6](deploy/desktop/README.md) / [Build a clean image](deploy/desktop/BUILD.md)

In LuCI, open **Status → Agent Routing → User mode**. Connect to Cloud or configure Router network, then enable Agent services. Cloud Direct IPv6 / Relay clients and self-hosted Open Mesh seeds are separate deployment paths. The Relay and Directory example configurations contain nonfunctional signing-key placeholders; generate your own keys and certificates before deployment. Follow the router roles guide to configure an OpenWrt seed.

## Build APK packages, install and run

For a first installation, follow the [complete installation guide](docs/user/en/getting-started/install.md), which includes official SDK download URLs, checksums, signing and troubleshooting. The build host is Linux x86_64; the router example uses OpenWrt 25.12.4 x86/64. **The default configuration cannot be used directly for ARM.**

1. **Identify the device:** run `ubus call system board` and `apk --print-arch` on the router, then select an SDK matching its version and target/subtarget.
2. **Build on your computer:** install the dependencies from the guide and place the source and a fresh SDK in Linux paths without spaces. From the repository root, run:

   ```sh
   JOBS=2 sh scripts/build-openwrt-sdk.sh /absolute/path/to/openwrt-sdk
   ```

   The script overwrites the SDK's `.config`. The default output is `bin/packages/x86_64/nexus_agent_router/` inside the SDK. CMake host tests do not produce APK packages.
3. **Sign and transfer the package feed:** follow the guide to collect the new APKs in `nexus-feed` and sign `packages.adb` with the SDK private key. Copy only the APKs, index, public key and SHA256SUMS to the router. Keep the private key on the build host.
4. **Install on the router:** compare the public-key digest, import the key into `/etc/apk/keys` and verify the index as described in the guide, then run:

   ```sh
   apk update
   apk --repository /root/nexus-feed/packages.adb add --simulate luci-app-agent-router
   apk --repository /root/nexus-feed/packages.adb add luci-app-agent-router
   ```

   Keep the matching official feeds for dependency resolution; do not disable signature checks. The LuCI package pulls in the core routing components and Cloud connector. If the complete LuCI web service is not installed, also run `apk add luci`.
5. **Start and verify:** follow the guide to enable `agent-netd`, `agentd`, `agent-gw` and `agent-adapter`, then reload rpcd. Open **Status → Agent Routing → User mode**, confirm a unique Router ID and enable **Agent services**. Check services with `ubus call agent stats` and `curl --fail http://127.0.0.1:7788/healthz`, then register and invoke a real Agent.

The basic helper does not build the `nexus-agent-router` profile metapackages or Node/Relay/Directory packages. Do not attempt to install `nexus-agent-router-seed` before building it. A basic Cloud client does not need a self-hosted Relay. Advanced roles and complete VM images require the additional builds and validation described in their respective guides.

## Host tests

On Linux, install a C17 compiler, CMake ≥ 3.16, Python 3, Node.js ≥ 20, and the OpenSSL development libraries and CLI. For example, on Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build python3 libssl-dev openssl
# Separately install Node.js >= 20; check node --version.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNEXUS_INTEGRATION_TESTS=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
npm --prefix relay test
python3 -m unittest discover -s deploy/desktop -p test_bundle.py
```

These tests cover the host routing core, configuration contracts and Relay/Directory protocols; they do not boot a complete OpenWrt system. `NEXUS_INTEGRATION_TESTS=OFF` explicitly excludes tests that depend on the independent SDK and internal lab orchestration. The full CMake contract retains those entry points for monorepo integration testing. Use a Debug build so optimization settings do not disable C `assert` checks.

## Contributing and licensing

[CONTRIBUTING](CONTRIBUTING.md) · [SECURITY](SECURITY.md) · [CHANGELOG](CHANGELOG.md) · [Release checks](RELEASING.md)

Nexus-authored code uses [Apache License 2.0 (modified)](LICENSE); see [NOTICE](NOTICE) for its scope. Third-party code and build dependencies retain their original licenses; see [THIRD_PARTY](THIRD_PARTY.md). In particular, the Node OpenWrt build recipe retains GPL v2: do not label an entire OpenWrt firmware image as Apache-2.0-only. Per-file source SHA-256 hashes are recorded in `SOURCE-MANIFEST.json`. This manifest provides export traceability, not a signature or security-audit certificate.

### Licensing conditions

Nexus is licensed under a modified version of the Apache License 2.0, with the following additional conditions. Multi-tenant service operation and removal of existing Nexus UI branding require prior written authorization. Earlier Apache-2.0 grants and third-party licenses remain unchanged. Contributions require explicit agreement permitting commercial use and future relicensing. See [LICENSING.md](LICENSING.md). Authorization contact: **cary.nexilume@outlook.com**.

---
sidebar_position: 1
title: Build APKs, install and start
---

# Build APKs, install and start

Build on a **Linux x86_64 computer**, then install on an **OpenWrt router**.
These examples pin **OpenWrt 25.12.4, x86/64**, our build baseline, not a claim
about the latest release. Use a new dedicated SDK. Host CMake tests do not build
OpenWrt APKs.

## 1. Identify the target (router SSH)

```sh
ubus call system board
cat /etc/openwrt_release
apk --print-arch
df -h /overlay /tmp
```

This example requires release `25.12.4`, target `x86/64`, APK architecture
`x86_64`. A different version, target/subtarget or architecture needs a matching
SDK and separate target validation. The opkg-based 24.10 workflow is different.
Keep a configuration backup and your SSH management connection. The router
needs access to matching official repositories for dependencies.

## 2. Prepare Linux (computer)

Use Linux x86_64 or WSL2 Ubuntu x86_64. On WSL, keep source and SDK under the
Linux home directory, not `/mnt/c`, `/mnt/d` or paths containing spaces. Run
these blocks in the same shell. Only system dependency installation uses sudo;
build as a regular user.

```sh
sudo apt-get update
sudo apt-get install -y build-essential gawk gettext git libncurses-dev   rsync unzip zlib1g-dev file wget curl python3 python3-setuptools   swig libssl-dev xsltproc zstd openssl ca-certificates
mkdir -p "$HOME/nexus-work"
cd "$HOME/nexus-work"
```

Extract or check out this source repository to `$HOME/nexus-work/nexus-openwrt`.
That directory must directly contain `feed/`, `scripts/` and `CMakeLists.txt`.
No GitHub clone URL has been published yet. Download and verify the pinned SDK;
its extraction directory must not exist:

```sh
set -eu
cd "$HOME/nexus-work"
SDK_ARCHIVE=openwrt-sdk-25.12.4-x86-64_gcc-14.3.0_musl.Linux-x86_64.tar.zst
curl --fail --location --proto '=https' --tlsv1.2   -o "$SDK_ARCHIVE" "https://downloads.openwrt.org/releases/25.12.4/targets/x86/64/$SDK_ARCHIVE"
printf '%s  %s\n'   28e004c1be4d215d19c1f12a6aa4c8d8f80689549eb707d0ff5a71f16fa8d05f   "$SDK_ARCHIVE" | sha256sum -c -
test ! -e "${SDK_ARCHIVE%.tar.zst}"
tar --zstd -xf "$SDK_ARCHIVE"
SDK="$HOME/nexus-work/${SDK_ARCHIVE%.tar.zst}"
REPO="$HOME/nexus-work/nexus-openwrt"
test -f "$REPO/feed/agentd/Makefile"
```

The filename and digest come from the [official target directory](https://downloads.openwrt.org/releases/25.12.4/targets/x86/64/).
Reset `SDK` and `REPO` if you open a new terminal.

## 3. Build base router packages (computer)

```sh
cd "$REPO"
JOBS=2 sh scripts/build-openwrt-sdk.sh "$SDK"
find "$SDK/bin/packages" -type f -name '*.apk' -print
```

Use `JOBS=1` if memory is limited. This helper updates dependency feeds,
**overwrites the SDK .config**, and builds the base packages. The default
`sdk/p8123-minimal.config` selects x86/64. Initial downloads and compilation can
take time.

Nexus APKs are under `$SDK/bin/packages/x86_64/nexus_agent_router`.
`luci-app-agent-router` depends on agentd, agent-netd, agent-gw, agent-adapter,
nexus-agent-roles and nexus-cloud-connector. agent-cardd is built separately and
is optional. Use actual output versions. Node.js is not needed for the base
profile.

For other architectures, prepare a configuration for that SDK's actual target
and pass `MINIMAL_CONFIG=/absolute/path/to/target.config JOBS=2 sh scripts/build-openwrt-sdk.sh "$SDK"`.
Also adjust the output architecture directory and validate on the target. Do
not apply the default x86 configuration to an ARM SDK.

## 4. Create a trusted local repository (computer)

SDK APKs may not have individual signatures. Generate a signed **packages.adb**
index and install by package name through that index. Copy only your fresh SDK's
Nexus feed into a new bundle directory:

```sh
set -eu
FEED="$SDK/bin/packages/x86_64/nexus_agent_router"
BUNDLE="$HOME/nexus-work/nexus-feed"
APK="$SDK/staging_dir/host/bin/apk"
test -s "$SDK/private-key.pem"
test -s "$SDK/public-key.pem"
set -- "$FEED"/agentd-*.apk
test -f "$1"
mkdir "$BUNDLE"
cp "$FEED"/*.apk "$BUNDLE/"
(
  cd "$BUNDLE"
  "$APK" mkndx --root "$SDK" --keys-dir "$SDK" --allow-untrusted     --sign "$SDK/private-key.pem" --output packages.adb ./*.apk
)
cp "$SDK/public-key.pem" "$BUNDLE/nexus-build-public.pem"
"$APK" --keys-dir "$BUNDLE" verify "$BUNDLE/packages.adb"
(
  cd "$BUNDLE"
  sha256sum ./*.apk packages.adb nexus-build-public.pem > SHA256SUMS
)
sha256sum "$BUNDLE/nexus-build-public.pem"
```

`mkndx --allow-untrusted` here permits the build host to index its own unsigned
APKs; the router installation does not disable verification. Record the public
key file SHA-256 printed last and compare it on the router before trusting it.
Never copy `private-key.pem` to the router or public repository. If the SDK has
not generated signing keys, complete its signing setup first.

## 5. Transfer, verify and install

On the computer, replace the example management address. The destination
`/root/nexus-feed` must not already exist. `scp -O` uses the SCP protocol supported
by Dropbear:

```sh
ROUTER=192.168.1.1
ssh "root@$ROUTER" 'test ! -e /root/nexus-feed'
scp -O -r "$BUNDLE" "root@$ROUTER:/root/"
ssh "root@$ROUTER"
```

In the router SSH shell:

```sh
set -eu
cd /root/nexus-feed
sha256sum -c SHA256SUMS
sha256sum nexus-build-public.pem
```

Compare the public key digest with the one recorded on your computer before
continuing. SHA256SUMS detects transfer corruption, not provenance by itself.
Then install the key, verify the signed index and simulate the installation
before running the final add command:

```sh
mkdir -p /etc/apk/keys
cp nexus-build-public.pem /etc/apk/keys/nexus-build-public.pem
chmod 0644 /etc/apk/keys/nexus-build-public.pem
apk verify /root/nexus-feed/packages.adb
apk update
apk --repository /root/nexus-feed/packages.adb add --simulate luci-app-agent-router
apk --repository /root/nexus-feed/packages.adb add luci-app-agent-router
apk info -e agentd agent-netd agent-gw agent-adapter nexus-agent-roles   nexus-cloud-connector luci-app-agent-router
```

Keep official repositories enabled for libc, TLS, ubus and other dependencies.
Do not fix dependency/version errors with `--force` or `--allow-untrusted`.
On a bare system without the complete LuCI web service, also run `apk add luci`.
Optional card service: `apk --repository /root/nexus-feed/packages.adb add agent-cardd`.

The temporary `--repository` argument leaves default repositories unchanged.
Keep the bundle for reinstalls. Build a new signed bundle for upgrades and repeat
the simulation and installation against the new index. Offline devices need a
complete dependency repository; this bundle is not an offline installer.

## 6. Start and configure (router)

```sh
for service in agent-netd agentd agent-gw agent-adapter; do
  /etc/init.d/"$service" enable
  /etc/init.d/"$service" restart
done
/etc/init.d/rpcd restart
```

Enabling init services does not enable every UCI feature. The adapter may remain
stopped until Agent services is enabled. Cloud, Relay and Directory connections
are not automatically established by installing base packages.

Open your existing LuCI management address and select **Status → Agent Routing
→ User mode**. Check a unique Router ID and Agent domain in **Developer mode →
Quick Setup**, then enable **Agent services** in User mode. Enable **Router
network** when LAN peering is needed. Local use does not require Cloud; configure
its HTTPS endpoint and pairing code only when connecting. Ordinary clients keep
**Router Roles → Hosted services → Node only**.
See [first configuration](quick-setup.md). Do not expose trusted-LAN SDK port
7446 or legacy No-JWT port 7445 directly to WAN.

## 7. Verify operation

```sh
ubus call agent stats
agentctl agents 10
agentctl routes 10
curl --fail http://127.0.0.1:7788/healthz
logread | grep -E 'agentd|agent-gw|agent-adapter' | tail -n 40
```

The default loopback gateway is `127.0.0.1:7788`; use your actual address if you
changed it. First confirm ubus status, successful healthz and healthy Agent
services in User mode. Then [publish and invoke an Agent](../guides/publish-api.md)
and verify a lease, capability route and real response. Empty routes, agents,
neighbors or Relay sessions on a fresh installation are not necessarily errors.

## Optional roles and troubleshooting

The base helper does not emit the `nexus-agent-router` metapackage or build Node,
Relay and Directory. Do not run `apk add nexus-agent-router-seed` without first
building `feed/nexus-agent-profiles` and its service dependencies. See the
[roles guide](../guides/router-roles.md) and [desktop image workflow](desktop-vm.md).

| Symptom | Check |
| --- | --- |
| Build failure | First error, regular user, Linux paths without spaces, network; use JOBS=1 for low memory |
| Missing APKs | Build must exit successfully; inspect bin/packages/*/nexus_agent_router, not the CMake directory |
| UNTRUSTED signature | SDK public key and signed index must match; check PEM in /etc/apk/keys; keep verification enabled |
| Architecture/dependency conflict | Match system board, apk architecture, SDK target and official repository version |
| Missing LuCI menu | Relogin, refresh, check luci-app-agent-router and rpcd; check complete LuCI on bare systems |
| Stopped service | Check UCI feature switches, User mode status and logs; init cannot override disabled configuration |
| Calls fail | Check registration, lease, authentication and reachability from Router to Agent callback |

References: [OpenWrt SDK](https://openwrt.org/docs/guide-developer/toolchain/using_the_sdk),
[APK](https://openwrt.org/docs/guide-user/additional-software/apk).
Commands have been checked against source and APK tooling; actual device
installation, upgrade and invocation acceptance remains necessary.

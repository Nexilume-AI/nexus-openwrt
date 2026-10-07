# Install the signed x86_64 package feed

This feed targets **OpenWrt 25.12.4, x86/64, APK architecture x86_64** only.
It is not a firmware image, an offline installer, or an opkg package set.
Keep a configuration backup and an SSH management connection. Do not use it
on ARM/MIPS routers or a different OpenWrt release.

## Choose one installation level

| Profile | Includes | Node.js installed? |
| --- | --- | --- |
| `nexus-agent-router` | Agent routing, LAN SDK registration, LuCI, Cloud Direct IPv6 / Relay **client** | No |
| `nexus-agent-router-relay` | Router plus a **self-hosted Open Mesh Relay** | Yes |
| `nexus-agent-router-seed` | Router plus self-hosted Relay and Directory | Yes |

One archive contains all three cumulative profiles. Installing the base profile
does not install Node.js or the optional servers. Installing a role package does
not automatically expose a Relay or pair with Cloud; configure those roles in
LuCI. Cloud enrollment, seed identity, certificates and credentials are not bundled.

## Verify, transfer and install

1. Download the archive, `SHA256SUMS`, and `nexus-release-public.pem` from the
   [same GitHub Release](https://github.com/Nexilume-AI/nexus-openwrt/releases).
   Verify the outer `SHA256SUMS` and compare the public-key SHA-256 with the release
   notes through a trusted channel. Checksums alone do not establish authenticity.
2. Extract into a **new directory**, and transfer that directory to the router
   using your existing SSH/SCP connection. The examples below use `/root/nexus-feed`.
3. On the router, confirm its target before changing anything:

   ```sh
   cat /etc/openwrt_release
   apk --print-arch
   df -h /overlay /tmp
   cd /root/nexus-feed
   sha256sum -c SHA256SUMS
   sha256sum nexus-release-public.pem
   ```

4. Only after verifying the public-key fingerprint, trust it and check the index:

   ```sh
   mkdir -p /etc/apk/keys
   cp nexus-release-public.pem /etc/apk/keys/nexus-release-public.pem
   chmod 0644 /etc/apk/keys/nexus-release-public.pem
   apk verify /root/nexus-feed/packages.adb
   apk update
   ```

5. Choose **one** profile from the table. Review the simulated transaction, then
   install with the same command without `--simulate`:

   ```sh
   PROFILE=nexus-agent-router
   apk --repository /root/nexus-feed/packages.adb add --simulate "$PROFILE"
   apk --repository /root/nexus-feed/packages.adb add "$PROFILE"
   ```

Keep matching official repositories enabled for dependencies. A minimal OpenWrt
installation may also need `apk add luci` for its web server. Never resolve
signature or dependency failures with `--allow-untrusted` or `--force`.
Use one release batch together; the Gateway and agentd share an IPC ABI.

## Start using the router

Open **Status → Agent Routing → User mode** in LuCI. Enable **Agent services**
to register local Agents, and **Router network** to enable Mesh networking.
Cloud pairing is optional. Trusted-LAN registration is an admission boundary:
do not expose its listener to WAN or untrusted Wi-Fi clients. Review Open Mesh
trust settings before allowing untrusted peers onto the network.

Check `ubus call agent stats`, service status and an actual Agent invocation.
Empty Agent/neighbor lists are expected before registration or discovery.
See the [manual](https://github.com/Nexilume-AI/nexus-openwrt/tree/main/docs/user/en).

## Upgrade and recovery

Download a new batch into a new directory, verify it, simulate the upgrade and
review the exact package versions before applying. Preserve UCI configuration;
do not replace it with a test router's configuration. Restart the affected
services together, and verify registration/invocation again. Keep the previous
signed feed and configuration backup until the upgrade is accepted.

This first package release is a **Beta**. Refer to its release notes for the
tests actually completed and limitations. Installation of a package alone is
not evidence that your WAN IPv6, TLS, firewall or Relay deployment is ready.

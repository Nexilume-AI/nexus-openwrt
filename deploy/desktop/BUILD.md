# Build a clean desktop release

This is a publisher workflow, not an end-user requirement. Build a fresh image;
never sanitize and redistribute an existing router VHDX. The launcher profile
is `nexus-openwrt-hyperv-v1`, OpenWrt x86/64 EFI, management LAN
`192.168.246.1/24`, host `192.168.246.2/24`, guest IPv6
`fd6e:6578:7573:246::1/64`, host `fd6e:6578:7573:246::2/64`,
DHCP/RA disabled, no WAN attached.

## Image recipe

1. Use the repository's accepted OpenWrt 25.12.4 x86/64 SDK/build baseline.
   Build the Nexus feed with `scripts/build-openwrt-sdk.sh`, including the
   `nexus-agent-router-seed` profile and its dependencies for the full desktop
   package set. Package availability must be checked; the SDK helper's minimal
   config alone does not select every profile.
2. Prepare the matching **ImageBuilder** and a trusted local package repository
   containing the resulting `.apk` files and their dependencies. Configure its
   repository trust keys; do not disable signature checks. Confirm Hyper-V
   storage/network drivers are present in the x86/64 target and inspect `make info`.
3. Apply this clean overlay with the standard
   [OpenWrt ImageBuilder workflow](https://openwrt.org/docs/guide-user/additional-software/imagebuilder):

```sh
# In the matching ImageBuilder; the custom signed package repository is configured.
make image PROFILE=generic \
  PACKAGES="luci nexus-agent-router-seed agent-cardd linuxptp" \
  FILES="/absolute/path/to/nexus_openwrt/deploy/desktop/files" \
  CONFIG_TARGET_ROOTFS_PARTSIZE=512
```

4. Select the resulting **combined-efi** x86/64 image, decompress it to a new
   file and convert it without booting the release source:

```sh
gzip -dc /absolute/path/to/combined-efi.img.gz > /new/output/nexus-desktop.img
qemu-img convert -f raw -O vhdx /new/output/nexus-desktop.img /new/output/nexus-desktop.vhdx
python3 /absolute/path/to/deploy/desktop/prepare-bundle.py \
  --image /new/output/nexus-desktop.vhdx --output /new/output/desktop-bundle
```

Use new output paths: these shell redirections are not safe for existing files.
The packager refuses an existing bundle directory and creates a SHA-256 manifest.
It validates file format/header and bytes, not image contents or absence of
secrets. Image production and real VM acceptance are separate checks; an
ImageBuilder success alone is insufficient to mark the release ready.

## Release evidence

- Publish only the unbooted image; acceptance boots a disposable **copy** through
  the launcher, generating unique guest identities on first use.
- Verify LuCI, Agent Router setup, host Agent registration, authenticated
  invocation, reboot persistence and graceful stop/start.
- Verify DHCP/RA isolation, no physical bridge/default-route changes, and
  checksum mismatch, duplicate VM, subnet collision and interrupted setup refusal.
- Verify IPv4 and ULA IPv6 management on the booted copy, and repeat
  `configure-ipv6.ps1` to confirm it reapplies the local configuration.
  Test upstream DHCPv6 separately on an explicitly selected upstream network;
  record actual assigned addresses/prefixes and external reachability.
- Confirm package versions and source revisions. Retain firmware/package
  manifests, checksums, build configuration, upstream license notices and the
  corresponding sources required by their licenses. Nexus Community License 1.0 covers Nexus's
  code as stated in `../../NOTICE`; it does not relicense OpenWrt firmware.
- Exclude root passwords, SSH host keys, device identity/realm state, TLS private
  keys, Relay tickets, Cloud enrollment and logs. Do not copy local `evidence`,
  build directories, runtime config or enterprise Server/TokenBank docs into a
  public source release.

The OpenWrt source repository is a separate release from Cloud Community. Review
its file allowlist and license inventory before publishing; do not export the
entire monorepo or reuse the Cloud-only release approval as OpenWrt approval.

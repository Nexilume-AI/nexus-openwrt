#!/bin/sh
# Build all three profiles in a dedicated, matching SDK; no router is changed.
set -eu
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /absolute/path/to/openwrt-25.12.4-x86-64-sdk" >&2
    exit 2
fi
sdk_dir="$1"
repo_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
case "$repo_dir" in *' '*) echo 'Use a Linux source path without spaces' >&2; exit 2;; esac
test -x "$sdk_dir/scripts/feeds"
test -f "$sdk_dir/Makefile"
cd "$sdk_dir"
test -f feeds.conf || cp feeds.conf.default feeds.conf
expected_feed="src-link nexus_agent_router $repo_dir/feed"
existing_feed="$(grep '^src-link nexus_agent_router ' feeds.conf || true)"
if [ -n "$existing_feed" ] && [ "$existing_feed" != "$expected_feed" ]; then
    echo 'SDK already points at another Nexus source. Use a dedicated SDK.' >&2
    exit 2
fi
test -n "$existing_feed" || printf '\n%s\n' "$expected_feed" >> feeds.conf
for feed in base packages luci nexus_agent_router; do
    ./scripts/feeds update "$feed"
done
./scripts/feeds install -a -p base
./scripts/feeds install -a -p packages
./scripts/feeds install -a -p luci
./scripts/feeds install -p nexus_agent_router \
    agent-netd agentd agent-gw agent-adapter nexus-cloud-connector luci-app-agent-router \
    nexus-agent-roles nexus-agent-relayd nexus-agent-directoryd nexus-node-runtime \
    nexus-agent-router nexus-agent-router-relay nexus-agent-router-seed
# Intentionally replaces .config: this script is for a dedicated SDK only.
cp "$repo_dir/sdk/p8123-minimal.config" .config
printf '%s\n' \
    'CONFIG_PACKAGE_nexus-node-runtime=m' \
    'CONFIG_PACKAGE_nexus-agent-relayd=m' \
    'CONFIG_PACKAGE_nexus-agent-directoryd=m' \
    'CONFIG_PACKAGE_nexus-agent-router=m' \
    'CONFIG_PACKAGE_nexus-agent-router-relay=m' \
    'CONFIG_PACKAGE_nexus-agent-router-seed=m' >> .config
make defconfig
make -j"${JOBS:-2}" package/feeds/nexus_agent_router/nexus-agent-profiles/compile V=s
echo 'All three profiles built; signing and target installation tests are still required.'

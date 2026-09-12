#!/bin/sh

set -eu

usage() {
	echo "Usage: $0 /absolute/path/to/openwrt-sdk"
}

if [ "$#" -ne 1 ]; then
	usage >&2
	exit 2
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(dirname -- "$script_dir")"
sdk_dir="$1"
minimal_config="${MINIMAL_CONFIG:-$project_dir/sdk/p8123-minimal.config}"

if [ ! -x "$sdk_dir/scripts/feeds" ] || [ ! -f "$sdk_dir/Makefile" ]; then
	echo "Not an OpenWrt SDK/buildroot: $sdk_dir" >&2
	exit 2
fi
if [ ! -f "$minimal_config" ]; then
	echo "Minimal OpenWrt config is missing: $minimal_config" >&2
	exit 2
fi

case "$project_dir" in
	*" "*)
		echo "The local feed path must not contain spaces: $project_dir" >&2
		exit 2
		;;
esac

feeds_conf="$sdk_dir/feeds.conf"
if [ ! -f "$feeds_conf" ]; then
	if [ ! -f "$sdk_dir/feeds.conf.default" ]; then
		echo "SDK has no feeds.conf or feeds.conf.default" >&2
		exit 2
	fi
	cp "$sdk_dir/feeds.conf.default" "$feeds_conf"
fi

expected_feed="src-link nexus_agent_router $project_dir/feed"
existing_feed="$(grep '^src-link nexus_agent_router ' "$feeds_conf" || true)"
if [ -n "$existing_feed" ] && [ "$existing_feed" != "$expected_feed" ]; then
	echo "nexus_agent_router already points elsewhere:" >&2
	echo "$existing_feed" >&2
	exit 2
fi
if [ -z "$existing_feed" ]; then
	printf '\n%s\n' "$expected_feed" >> "$feeds_conf"
fi

jobs="${JOBS:-1}"
cd "$sdk_dir"
./scripts/feeds update base
./scripts/feeds update packages
./scripts/feeds update luci
./scripts/feeds update nexus_agent_router
./scripts/feeds install -p base \
	libubus libubox libuci procd-ujail \
	umdns \
	libuhttpd-nossl libjson-c libmbedtls \
	uclient-fetch ca-bundle libustream-mbedtls usign jshn openssl-util
./scripts/feeds install -p packages libev stunnel curl
./scripts/feeds install -p luci luci-base
./scripts/feeds install -p nexus_agent_router \
	agent-netd agentd agent-cardd agent-gw agent-adapter nexus-agent-roles \
	nexus-cloud-connector luci-app-agent-router
cp "$minimal_config" .config
make defconfig
# luci-app-agent-router pulls agentd, agent-netd, agent-gw, agent-adapter,
# nexus-agent-roles, and nexus-cloud-connector through its package dependency
# graph. Keep agent-cardd explicit because it is intentionally independent.
make -j"$jobs" package/agent-cardd/compile package/luci-app-agent-router/compile V=s

echo "Nexus Agent Router and cloud connector packages compiled successfully."

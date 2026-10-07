#!/bin/sh

set -eu

sdk_dir="${1:-/opt/openwrt/sdk}"
project_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
repo_dir="${2:-$project_dir}"
artifact_dir="${3:-$repo_dir/.tmp/open-mesh-e2e/artifacts}"
log_file="${4:-$repo_dir/.tmp/open-mesh-e2e/open-mesh-seed-build.log}"
reuse_dependencies="${NEXUS_SDK_REUSE_DEPENDENCIES:-}"
role_version="$(sed -n 's/^PKG_VERSION:=//p' "$repo_dir/feed/nexus-agent-services/Makefile")-r$(sed -n 's/^PKG_RELEASE:=//p' "$repo_dir/feed/nexus-agent-services/Makefile")"
case "$reuse_dependencies" in ''|1) ;; *) exit 2 ;; esac

test -f "$sdk_dir/Makefile"
test -f "$repo_dir/feed/nexus-node-runtime/Makefile"
test -f "$repo_dir/feed/nexus-agent-services/Makefile"
mkdir -p "$artifact_dir" "$(dirname "$log_file")"

cd "$sdk_dir"
ln -sfn "$repo_dir/feed" "$sdk_dir/feeds/nexus_agent_router"
./scripts/feeds update -i nexus_agent_router
for package in nexus-node-runtime nexus-agent-roles nexus-agent-relayd nexus-agent-directoryd; do
	./scripts/feeds install -f -p nexus_agent_router "$package"
done
cp "$repo_dir/sdk/p8123-minimal.config" "$sdk_dir/.config"
sed -i \
	-e 's/^CONFIG_PACKAGE_agent-netd=y$/# CONFIG_PACKAGE_agent-netd is not set/' \
	-e 's/^CONFIG_PACKAGE_agentd=y$/# CONFIG_PACKAGE_agentd is not set/' \
	-e 's/^CONFIG_PACKAGE_agent-gw=y$/# CONFIG_PACKAGE_agent-gw is not set/' \
	-e 's/^CONFIG_PACKAGE_agent-adapter=y$/# CONFIG_PACKAGE_agent-adapter is not set/' \
	-e 's/^CONFIG_PACKAGE_nexus-cloud-connector=y$/# CONFIG_PACKAGE_nexus-cloud-connector is not set/' \
	-e 's/^CONFIG_PACKAGE_luci-app-agent-router=y$/# CONFIG_PACKAGE_luci-app-agent-router is not set/' \
	"$sdk_dir/.config"
printf '%s\n' \
	'CONFIG_PACKAGE_nexus-node-runtime=m' \
	'CONFIG_PACKAGE_nexus-agent-relayd=m' \
	'CONFIG_PACKAGE_nexus-agent-directoryd=m' >>"$sdk_dir/.config"
make defconfig

: >"$log_file"
node_apk="$sdk_dir/bin/packages/x86_64/nexus_agent_router/nexus-node-runtime-20.20.2-r1.apk"
if [ "$reuse_dependencies" = 1 ]; then test -s "$node_apk"; fi
if [ ! -f "$node_apk" ]; then
	make -j4 package/feeds/nexus_agent_router/nexus-node-runtime/compile \
		V=sc >>"$log_file" 2>&1
fi
service_target='package/feeds/nexus_agent_router/nexus-agent-services'
make "$service_target/clean" >>"$log_file" 2>&1
make -j4 "$service_target/compile" V=sc NO_DEPS="$reuse_dependencies" >>"$log_file" 2>&1

copy_package() {
	pattern="$1"
	package="$(find "$sdk_dir/bin" -type f -name "$pattern" -print | sort | tail -n 1)"
	test -n "$package"
	cp "$package" "$artifact_dir/"
}

copy_package 'nexus-node-runtime-20.20.2-r1.apk'
copy_package "nexus-agent-roles-$role_version.apk"
copy_package "nexus-agent-relayd-$role_version.apk"
copy_package "nexus-agent-directoryd-$role_version.apk"
copy_package 'libstdcpp6-*.apk'
copy_package 'libatomic1-*.apk'
copy_package 'libcares-*.apk'

sha256sum "$artifact_dir"/nexus-node-runtime-20.20.2-r1.apk \
	"$artifact_dir"/nexus-agent-roles-"$role_version".apk \
	"$artifact_dir"/nexus-agent-relayd-"$role_version".apk \
	"$artifact_dir"/nexus-agent-directoryd-"$role_version".apk

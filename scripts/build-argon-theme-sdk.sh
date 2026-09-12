#!/bin/sh

set -eu

project_dir="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
sdk_dir="${1:-/opt/openwrt/sdk}"
artifact_dir="${2:-$project_dir/.tmp/p9.0/apks}"
minimal_config="${3:-$project_dir/sdk/p8123-minimal.config}"
cache_dir="${4:-$project_dir/.tmp/dependencies}"
lock_file="$project_dir/dependencies/luci-theme-argon.lock"
notice_file="$project_dir/dependencies/luci-theme-argon.NOTICE.md"

[ -f "$sdk_dir/Makefile" ] && [ -x "$sdk_dir/scripts/feeds" ] || {
	echo "Not an OpenWrt SDK: $sdk_dir" >&2
	exit 2
}
[ -f "$minimal_config" ] || {
	echo "Minimal SDK config is missing: $minimal_config" >&2
	exit 2
}
[ -f "$lock_file" ] && [ -f "$notice_file" ] || {
	echo 'Argon dependency lock or notice is missing.' >&2
	exit 2
}

# The lock is project-controlled and intentionally contains assignments only.
# shellcheck source=/dev/null
. "$lock_file"

case "$ARGON_TAG:$ARGON_SOURCE_SHA256:$ARGON_PACKAGE_VERSION:$ARGON_PACKAGE_RELEASE" in
	v[0-9]*:[0-9a-f][0-9a-f]*:[0-9]*:[0-9]*) ;;
	*) echo 'Argon dependency lock has an invalid value.' >&2; exit 2 ;;
esac
[ "${#ARGON_SOURCE_SHA256}" -eq 64 ] || {
	echo 'Argon source SHA-256 must contain 64 hexadecimal characters.' >&2
	exit 2
}
case "$ARGON_SOURCE_SHA256" in
	*[!0-9a-f]*) echo 'Argon source SHA-256 contains a non-hexadecimal character.' >&2; exit 2 ;;
esac
[ "$ARGON_CONFIG_APP" = 'disabled' ] || {
	echo 'The Nexus profile intentionally excludes luci-app-argon-config.' >&2
	exit 2
}
[ -f "$sdk_dir/private-key.pem" ] && [ -f "$sdk_dir/public-key.pem" ] || {
	echo 'The SDK APK signing key pair is missing.' >&2
	exit 2
}

mkdir -p "$cache_dir" "$artifact_dir"
archive="$cache_dir/$ARGON_NAME-$ARGON_TAG.tar.gz"
if [ ! -f "$archive" ]; then
	download="$archive.download"
	curl --fail --location --proto '=https' --tlsv1.2 \
		--output "$download" "$ARGON_SOURCE_URL"
	mv "$download" "$archive"
fi
printf '%s  %s\n' "$ARGON_SOURCE_SHA256" "$archive" | sha256sum -c -

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/nexus-argon.XXXXXX")"
feeds_file="$sdk_dir/feeds.conf"
feeds_backup="$work_dir/feeds.conf"
nexus_feed_link="$sdk_dir/feeds/nexus_agent_router"
nexus_feed_original="$(readlink "$nexus_feed_link" 2>/dev/null || true)"
argon_feed_link="$sdk_dir/feeds/nexus_argon"
argon_package_link="$sdk_dir/package/feeds/nexus_argon"
owns_argon_feed=0

restore_sdk() {
	cp "$feeds_backup" "$feeds_file"
	if [ -n "$nexus_feed_original" ]; then
		ln -sfn "$nexus_feed_original" "$nexus_feed_link"
	elif [ -L "$nexus_feed_link" ]; then
		rm -f "$nexus_feed_link"
	fi
	if [ "$owns_argon_feed" -eq 1 ]; then
		[ ! -L "$argon_feed_link" ] || rm -f "$argon_feed_link"
		[ ! -d "$argon_package_link" ] || rm -rf "$argon_package_link"
	fi
	rm -rf "$work_dir"
}
cp "$feeds_file" "$feeds_backup"
trap restore_sdk EXIT HUP INT TERM

[ ! -e "$argon_feed_link" ] && [ ! -e "$argon_package_link" ] || {
	echo 'SDK already contains a nexus_argon feed; refusing to overwrite it.' >&2
	exit 2
}

tar -xzf "$archive" -C "$work_dir"
argon_extracted="$work_dir/$ARGON_SOURCE_DIR"
[ -f "$argon_extracted/Makefile" ] && [ -f "$argon_extracted/LICENSE" ] || {
	echo 'Argon source archive does not contain the expected package.' >&2
	exit 1
}
argon_source="$work_dir/$ARGON_NAME"
mv "$argon_extracted" "$argon_source"
grep -q '^                                 Apache License$' "$argon_source/LICENSE"
grep -q "^PKG_VERSION:=$ARGON_UPSTREAM_PKG_VERSION$" "$argon_source/Makefile"
grep -q "^PKG_RELEASE:=$ARGON_UPSTREAM_PKG_RELEASE$" "$argon_source/Makefile"

# v2.4.6 is tagged with stale 2.4.5 package metadata. Normalize only inside the
# temporary verified source tree so the resulting package identity matches the
# upstream release. Add a neutral rendered color-scheme marker for Nexus CSS.
sed -i "s/^PKG_VERSION:=.*/PKG_VERSION:=$ARGON_PACKAGE_VERSION/" "$argon_source/Makefile"
sed -i "s/^PKG_RELEASE:=.*/PKG_RELEASE:=$ARGON_PACKAGE_RELEASE/" "$argon_source/Makefile"
for header in \
	"$argon_source/ucode/template/themes/argon/header.ut" \
	"$argon_source/ucode/template/themes/argon/header_login.ut"; do
	sed -i 's|<html lang="{{ dispatcher.lang }}">|<html lang="{{ dispatcher.lang }}" data-nexus-color-scheme="{{ mode }}">|' "$header"
	grep -q 'data-nexus-color-scheme="{{ mode }}"' "$header"
done

feed_root="$work_dir/feed"
mkdir -p "$feed_root"
ln -s "$argon_source" "$feed_root/$ARGON_NAME"
printf '\nsrc-link nexus_argon %s\n' "$feed_root" >>"$feeds_file"
sed -i "s|^src-link nexus_agent_router .*|src-link nexus_agent_router $project_dir/feed|" "$feeds_file"
ln -s "$feed_root" "$argon_feed_link"
owns_argon_feed=1
ln -sfn "$project_dir/feed" "$nexus_feed_link"

cd "$sdk_dir"
./scripts/feeds update -i nexus_argon >/dev/null
./scripts/feeds install -f -p nexus_argon luci-theme-argon >/dev/null
./scripts/feeds update -i nexus_agent_router >/dev/null
./scripts/feeds install -f -p nexus_agent_router \
	nexus-luci-theme-profile luci-app-agent-router >/dev/null

cp "$minimal_config" .config
cat >>.config <<'EOF'
CONFIG_PACKAGE_luci-theme-argon=m
CONFIG_PACKAGE_luci-theme-bootstrap=y
CONFIG_PACKAGE_nexus-luci-theme-profile=m
CONFIG_PACKAGE_luci-app-agent-router=m
CONFIG_SIGNED_PACKAGES=y
EOF
make defconfig >/dev/null

log_file="$artifact_dir/build.log"
: >"$log_file"
make package/luci-theme-argon/clean \
	package/nexus-luci-theme-profile/clean \
	package/luci-app-agent-router/clean >/dev/null
make package/luci-theme-argon/compile \
	package/nexus-luci-theme-profile/compile \
	package/luci-app-agent-router/compile -j2 V=sc \
	CONFIG_PACKAGE_luci-theme-argon=m \
	CONFIG_PACKAGE_nexus-luci-theme-profile=m \
	CONFIG_PACKAGE_luci-app-agent-router=m \
	>>"$log_file" 2>&1

publish_dir="$work_dir/publish"
mkdir -p "$publish_dir"
for artifact in \
	"luci-theme-argon-$ARGON_PACKAGE_VERSION-r$ARGON_PACKAGE_RELEASE.apk" \
	'nexus-luci-theme-profile-1.0.0-r1.apk' \
	'luci-app-agent-router-3.1.0-r12.apk'; do
	apk_file="$(find "$sdk_dir/bin/packages" -type f -name "$artifact" -print -quit)"
	[ -n "$apk_file" ] || {
		echo "Argon integration APK output missing: $artifact" >&2
		exit 1
	}
	cp "$apk_file" "$publish_dir/"
done

apk_tool="$sdk_dir/staging_dir/host/bin/apk"
(cd "$publish_dir" && "$apk_tool" mkndx \
	--root "$sdk_dir" --keys-dir "$sdk_dir" --allow-untrusted \
	--sign "$sdk_dir/private-key.pem" --output packages.adb ./*.apk)
cp "$sdk_dir/public-key.pem" "$publish_dir/nexus-argon-feed-public-key.pem"
cp "$lock_file" "$notice_file" "$publish_dir/"
"$apk_tool" --keys-dir "$sdk_dir" verify "$publish_dir/packages.adb"

key_fingerprint="$(openssl pkey -pubin -in "$sdk_dir/public-key.pem" -outform DER 2>/dev/null | sha256sum | cut -d' ' -f1)"
cat >"$publish_dir/provenance.json" <<EOF
{
  "component": "$ARGON_NAME",
  "upstream_repository": "$ARGON_REPOSITORY",
  "upstream_tag": "$ARGON_TAG",
  "source_url": "$ARGON_SOURCE_URL",
  "source_sha256": "$ARGON_SOURCE_SHA256",
  "package_version": "$ARGON_PACKAGE_VERSION-r$ARGON_PACKAGE_RELEASE",
  "license": "$ARGON_LICENSE",
  "config_app_included": false,
  "feed_index": "packages.adb",
  "feed_public_key_sha256": "$key_fingerprint"
}
EOF
(cd "$publish_dir" && sha256sum ./*.apk packages.adb \
	nexus-argon-feed-public-key.pem >SHA256SUMS)
cp "$publish_dir"/* "$artifact_dir/"

echo "Argon SDK build PASS: $artifact_dir"

#!/bin/sh

# /var is volatile on OpenWrt. Provision on EVERY enabled service start, before
# procd drops privileges, not only at package installation or seed enrollment.
# Never chown a configurable path, a symlink, or any directory recursively.
nexus_role_prepare_runtime() (
	local directory user
	case "$1" in
		relay) directory=/var/lib/nexus-relayd; user=nexus-relay ;;
		directory) directory=/var/lib/nexus-directoryd; user=nexus-directory ;;
		*) return 1 ;;
	esac
	umask 077
	if [ -L /var/lib ] || [ -L "$directory" ] ||
		{ [ -e /var/lib ] && [ ! -d /var/lib ]; } ||
		{ [ -e "$directory" ] && [ ! -d "$directory" ]; }; then
		echo "nexus-$1: ROLE_RUNTIME_DIRECTORY_UNSAFE: refusing redirected runtime directory" >&2
		return 1
	fi
	if { [ -d /var/lib ] || mkdir -m 0755 /var/lib; } &&
		{ [ -d "$directory" ] || mkdir -m 0700 "$directory"; } &&
		chown "$user:$user" "$directory" && chmod 0700 "$directory"; then
		return 0
	fi
	echo "nexus-$1: ROLE_RUNTIME_DIRECTORY_FAILED: check storage and service account" >&2
	return 1
)

#!/bin/sh
# Public routing information, NOT a credential or Cloud enrollment. Open Mesh
# is a deliberately open trust realm and never imports Cloud identity/trust.
mesh_ipv4_valid() {
	case "$1" in ''|*[!0-9.]*) return 1 ;; esac
	printf '%s\n' "$1" | awk -F. 'NF!=4 {exit 1} {for(i=1;i<=4;i++) if($i !~ /^[0-9]+$/ || $i>255) exit 1; if($1==0 || $1==127 || $1>=224) exit 1}'
}

# Shared read-only prerequisite check. Do not expose command output, paths,
# keys or raw role JSON through the UI. Re-run under the write lock on apply.
mesh_seed_preflight() {
	MESH_SETUP_CODE=READY
	if ! command -v node >/dev/null 2>&1 || ! command -v openssl >/dev/null 2>&1 ||
		! command -v timeout >/dev/null 2>&1 ||
		[ ! -x /etc/init.d/nexus-relayd ] || [ ! -x /etc/init.d/nexus-directoryd ] ||
		! id nexus-relay >/dev/null 2>&1 || ! id nexus-directory >/dev/null 2>&1; then
		MESH_SETUP_CODE=SEED_COMPONENTS_MISSING; return 1
	fi
	if [ -n "$(uci changes nexus_roles)$(uci changes firewall)$(uci changes network)" ]; then
		MESH_SETUP_CODE=CONFIGURATION_CHANGED; return 1
	fi
	if mesh_seed_status >/dev/null 2>&1; then
		MESH_SETUP_CODE=SEED_ALREADY_CONFIGURED; return 0
	fi
	if [ -e /etc/nexus-open-mesh-seed ]; then
		MESH_SETUP_CODE=SEED_IDENTITY_INCOMPLETE; return 1
	fi
	local config
	for config in /etc/nexus-relayd/relay.json /etc/nexus-directoryd/directory.json; do
		if [ -L "$config" ] || { [ -s "$config" ] && ! grep -q 'GENERATE_SHARED_TICKET_KEY' "$config"; }; then
			MESH_SETUP_CODE=CUSTOM_ROLE_CONFIGURATION; return 1
		fi
	done
	if [ -n "$(uci -q get firewall.nexus_open_mesh_relay || true)$(uci -q get firewall.nexus_open_mesh_directory || true)" ]; then
		MESH_SETUP_CODE=CUSTOM_ROLE_CONFIGURATION; return 1
	fi
	if ! uci show firewall 2>/dev/null | grep -Eq "\.name='(wan|nexuswan)'$"; then
		MESH_SETUP_CODE=SEED_FIREWALL_UNAVAILABLE; return 1
	fi
}

mesh_link_parse() {
	local payload decoded
	MESH_PROFILE=''
	[ "${#1}" -le 5500 ] || return 1
	case "$1" in nexus-mesh://join/*) payload="${1#nexus-mesh://join/}" ;; *) return 1 ;; esac
	case "$payload" in ''|*[!A-Za-z0-9_-]*) return 1 ;; esac
	case $((${#payload} % 4)) in 0) ;; 2) payload="$payload==" ;; 3) payload="$payload=" ;; *) return 1 ;; esac
	decoded="$(printf '%s' "$payload" | tr '_-' '/+' | openssl base64 -d -A 2>/dev/null)" || return 1
	if [ "$(jsonfilter -s "$decoded" -e '@.v' 2>/dev/null)" = 2 ]; then
		[ -x /usr/sbin/agent-mesh-profile ] || return 1
		MESH_PROFILE="$(printf '%s' "$decoded" | /usr/sbin/agent-mesh-profile)" || return 1
		MESH_ENDPOINT="https://directory-seed.mesh.local:$(jsonfilter -s "$MESH_PROFILE" -e '@.paths[0].directory_port')/v1/open-mesh/assignment"
		MESH_CONNECT=''
		return 0
	fi
	[ "$(jsonfilter -s "$decoded" -e '@.v' 2>/dev/null)" = 1 ] || return 1
	MESH_ENDPOINT="$(jsonfilter -s "$decoded" -e '@.directory' 2>/dev/null)"
	MESH_CONNECT="$(jsonfilter -s "$decoded" -e '@.connect' 2>/dev/null)"
	case "$MESH_ENDPOINT" in *[!A-Za-z0-9.:/-]*) return 1 ;; esac
	case "$MESH_CONNECT" in ''|*[!0-9.]*) return 1 ;; esac
	# Single fixed path, no user-info, redirects, fragments, queries or shell data.
	printf '%s\n' "$MESH_ENDPOINT" | grep -Eq '^https://[A-Za-z0-9][A-Za-z0-9.-]*:[0-9]{1,5}/v1/open-mesh/assignment$' || return 1
	local port="${MESH_ENDPOINT%/v1/open-mesh/assignment}"
	port="${port##*:}"
	[ "$port" -gt 0 ] && [ "$port" -le 65535 ] || return 1
	printf '%s\n' "$MESH_CONNECT" | awk -F. 'NF != 4 {exit 1} {for(i=1;i<=4;i++) if($i !~ /^[0-9]+$/ || $i>255) exit 1; if($1==0 || $1==127 || $1>=224) exit 1}' || return 1
}

mesh_seed_status() {
	# Only known fields are read; never return ticket keys or TLS paths.
	local endpoint connect port encoded
	[ -f /etc/nexus-open-mesh-seed/ca.pem ] || return 1
	[ "$(jsonfilter -i /etc/nexus-directoryd/directory.json -e '@.openMesh.enabled' 2>/dev/null)" = true ] || return 1
	[ "$(jsonfilter -i /etc/nexus-relayd/relay.json -e '@.openMesh.enabled' 2>/dev/null)" = true ] || return 1
	if [ -x /usr/sbin/agent-mesh-profile ] && [ -f /usr/lib/nexus-agent-roles/mesh-profile.js ]; then
		local profile paths
		paths="$(uci -q get nexus_roles.main.mesh_share_paths || true)"
		profile="$(printf '%s' "${paths:-[]}" | node /usr/lib/nexus-agent-roles/mesh-profile.js | /usr/sbin/agent-mesh-profile)" || return 1
		encoded="$(printf '%s' "$profile" | openssl base64 -A | tr '+/' '-_' | tr -d '=')"
		printf '{"configured":true,"link_version":2,"join_link":"nexus-mesh://join/%s","paths":%s}\n' "$encoded" "$(jsonfilter -s "$profile" -e '@.paths')"
		return 0
	fi
	connect="$(jsonfilter -i /etc/nexus-directoryd/directory.json -e '@.relays["open-mesh-relay"].connectIpv4' 2>/dev/null)"
	port="$(uci -q get nexus_roles.directory.public_port 2>/dev/null || true)"
	[ -n "$port" ] || port="$(jsonfilter -i /etc/nexus-directoryd/directory.json -e '@.port' 2>/dev/null)"
	case "$port" in ''|*[!0-9]*) return 1 ;; esac
	case "$connect" in ''|*[!0-9.]*) return 1 ;; esac
	endpoint="https://directory-seed.mesh.local:$port/v1/open-mesh/assignment"
	encoded="$(printf '{"v":1,"directory":"%s","connect":"%s"}' "$endpoint" "$connect" | openssl base64 -A | tr '+/' '-_' | tr -d '=')"
	mesh_link_parse "nexus-mesh://join/$encoded" || return 1
	printf '{"configured":true,"join_link":"nexus-mesh://join/%s"}\n' "$encoded"
}

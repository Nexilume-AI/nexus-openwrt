#!/bin/sh
# LAN bootstrap is useful before Cloud enrollment. A device-local public key
# with no retained private key makes JWT rejection explicit; enrollment later
# selects its real verifier without changing this LAN-only trust boundary.
prepare_lan_only_auth() {
	[ "$lan_sdk_enabled" -eq 1 ] || return 0
	[ "$auth_mode" = none ] || return 0
	auth_mode=jwt
	jwt_required=1
	jwt_jwks_file=''
	jwt_public_key=/etc/agent-gw/lan-only-public.pem
	[ -s "$jwt_public_key" ] && [ ! -L "$jwt_public_key" ] && return 0
	[ ! -e "$jwt_public_key" ] && [ ! -L "$jwt_public_key" ] || return 1
	mkdir -p /etc/agent-gw || return 1
	local tmp
	tmp="$(mktemp /etc/agent-gw/lan-public.XXXXXX)" || return 1
	# openssl pkey rejects empty/failed key generation. The private key exists
	# only in the pipe, never on disk, in argv, config, logs or the response.
	if ! openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 2>/dev/null |
		openssl pkey -pubout -out "$tmp" 2>/dev/null; then
		rm -f "$tmp"
		return 1
	fi
	chown agent-gw:agentd "$tmp" && chmod 0640 "$tmp" &&
		mv "$tmp" "$jwt_public_key"
}

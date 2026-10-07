# Sourced by the authenticated LuCI facade. No network access, logging or eval.
# The caller supplies a private temporary directory and removes it afterwards.

pairing_https_origin() {
	local authority host port
	case "$1" in https://*) authority="${1#https://}" ;; *) return 1 ;; esac
	[ "${#authority}" -le 260 ] || return 1
	case "$authority" in *[[:space:]]*) return 1 ;; esac
	# Canonical DNS/IPv4 or bracketed IPv6, with an optional bounded port.
	printf '%s\n' "$authority" | grep -Eq '^([A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?|\[[0-9A-Fa-f:]+\])(:[0-9]{1,5})?$' || return 1
	case "$authority" in
		\[*\]:*) port="${authority##*:}" ;;
		\[*\]) return 0 ;;
		*:*) port="${authority##*:}" ;;
		*) return 0 ;;
	esac
	[ "$port" -ge 1 ] && [ "$port" -le 65535 ]
}

pairing_read_trust() {
	local field="$1" directory="$2" mode pem digest actual type cert count
	json_get_type type "$field"
	[ "$type" = object ] && json_select "$field" || return 1
	json_get_var mode mode
	case "$mode" in
		system)
			json_get_type type ca_pem
			[ -z "$type" ] || return 1
			printf '%s' /etc/ssl/certs/ca-certificates.crt >"$directory/$field.path"
			;;
		pinned-pem)
			json_get_type type ca_pem
			[ "$type" = string ] || return 1
			json_get_var pem ca_pem
			# jshn versions differ in whether terminal newlines survive extraction.
			# The Cloud wire format is canonical PEM with exactly one final LF.
			pem="$(printf '%s' "$pem")"
			json_get_var digest sha256
			[ "${#pem}" -le 12288 ] && [ "${#digest}" -eq 64 ] || return 1
			case "$digest" in *[!0-9a-f]*) return 1 ;; esac
			printf '%s\n' "$pem" >"$directory/$field.pem"
			actual="$(sha256sum "$directory/$field.pem" | cut -d ' ' -f 1)"
			[ "$actual" = "$digest" ] || return 1
			# Reject private keys, prose, partial PEM and every non-certificate block.
			awk -v dir="$directory" -v prefix="$field" '
				/^-----BEGIN CERTIFICATE-----$/ { if (inside) exit 1; inside=1; n++; if (n>8) exit 1 }
				{ if (inside) print > (dir "/" prefix "-" n ".crt") }
				/^-----END CERTIFICATE-----$/ { if (!inside) exit 1; inside=0; next }
				/^-----BEGIN CERTIFICATE-----$/ { next }
				inside { if ($0 !~ /^[A-Za-z0-9+\/=]+$/) exit 1; next }
				/[^[:space:]]/ { exit 1 }
				END { if (inside || !n) exit 1 }
			' "$directory/$field.pem" || return 1
			for cert in "$directory/$field-"*.crt; do
				openssl x509 -in "$cert" -noout -checkend 0 >/dev/null 2>&1 || return 1
				openssl x509 -in "$cert" -noout -ext basicConstraints 2>/dev/null | grep -q 'CA:TRUE' || return 1
			done
			printf '/etc/agent-gw/pairing-%s.pem' "$digest" >"$directory/$field.path"
			;;
		*) return 1 ;;
	esac
	json_select ..
}

parse_pairing_link() {
	local link="$1" directory="$2" encoded padded decoded canonical version expiry type now
	PAIRING_ERROR=INVALID_PAIRING_LINK
	[ "${#link}" -le 49152 ] || return 1
	case "$link" in nexus-router://pair/v1/*) encoded="${link#nexus-router://pair/v1/}" ;; *) return 1 ;; esac
	case "$encoded" in ''|*[!A-Za-z0-9_-]*) return 1 ;; esac
	padded="$(printf '%s' "$encoded" | tr '_-' '/+')"
	case $((${#encoded} % 4)) in 0) ;; 2) padded="$padded==" ;; 3) padded="$padded=" ;; *) return 1 ;; esac
	decoded="$(printf '%s' "$padded" | openssl base64 -A -d 2>/dev/null)" || return 1
	canonical="$(printf '%s' "$decoded" | openssl base64 -A | tr '/+' '_-' | tr -d '=')"
	[ "$canonical" = "$encoded" ] || return 1
	json_load "$decoded" >/dev/null 2>&1 || return 1
	json_get_type type version
	json_get_var version version
	[ "$type" = int ] && [ "$version" = 1 ] || return 1
	json_get_var PAIRING_ORIGIN cloud
	pairing_https_origin "$PAIRING_ORIGIN" || return 1
	json_get_type type code
	[ "$type" = string ] || return 1
	json_get_var PAIRING_CODE code
	case "$PAIRING_CODE" in pair_*) ;; *) return 1 ;; esac
	case "$PAIRING_CODE" in *[!A-Za-z0-9._~-]*) return 1 ;; esac
	[ "${#PAIRING_CODE}" -le 256 ] || return 1
	json_get_type type expires_at
	json_get_var expiry expires_at
	[ "$type" = int ] && [ "${#expiry}" -eq 10 ] || return 1
	case "$expiry" in *[!0-9]*) return 1 ;; esac
	now="$(date +%s)"
	if [ "$expiry" -le "$now" ]; then PAIRING_ERROR=PAIRING_LINK_EXPIRED; return 1; fi
	pairing_read_trust trust "$directory" && pairing_read_trust edge_trust "$directory" || return 1
	PAIRING_PUBLIC_CA="$(cat "$directory/trust.path")"
	PAIRING_EDGE_CA="$(cat "$directory/edge_trust.path")"
}

install_pairing_trust() {
	local directory="$1" field target
	mkdir -p /etc/agent-gw || return 1
	for field in trust edge_trust; do
		[ -f "$directory/$field.pem" ] || continue
		target="$(cat "$directory/$field.path")"
		[ ! -L "$target" ] && [ ! -L "$target.tmp" ] || return 1
		# These are validated public CA certificates, never device private keys.
		# The unprivileged agent-gw must read them inside its jail for SDK trust
		# delivery. Keep root ownership and prohibit group/other writes.
		(umask 077; cp "$directory/$field.pem" "$target.tmp" &&
			chmod 0644 "$target.tmp" && mv -f "$target.tmp" "$target") || return 1
	done
}

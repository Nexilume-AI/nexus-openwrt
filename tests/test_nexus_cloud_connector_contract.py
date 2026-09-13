#!/usr/bin/env python3
"""Static contract checks for the Nexus Cloud Edge connector."""

from __future__ import annotations

import json
import sys
from pathlib import Path


root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
package = root / "feed" / "nexus-cloud-connector"
daemon = (package / "files" / "nexus-cloud-connectord").read_text(encoding="utf-8")
init_script = (package / "files" / "nexus-cloud.init").read_text(encoding="utf-8")
config = (package / "files" / "nexus-cloud.config").read_text(encoding="utf-8")
tls_check = (package / "files" / "nexus-cloud-tls-check").read_text(encoding="utf-8")
rpcd = (package / "files" / "nexus-cloud-rpcd").read_text(encoding="utf-8")
makefile = (package / "Makefile").read_text(encoding="utf-8")
cloud_ui = (
    root
    / "feed/luci-app-agent-router/htdocs/luci-static/resources/view/agent-router/cloud.js"
).read_text(encoding="utf-8")
build_script = (root / "scripts/build-openwrt-sdk.sh").read_text(encoding="utf-8")
agentd = (root / "feed/agentd/src/agentd.c").read_text(encoding="utf-8")
relay_invoke = (root / "feed/agentd/src/agent_relay_invoke.c").read_text(encoding="utf-8")
agentd_init = (root / "feed/agentd/files/agentd.init").read_text(encoding="utf-8")
menu = json.loads(
    (
        root
        / "feed/luci-app-agent-router/root/usr/share/luci/menu.d/luci-app-agent-router.json"
    ).read_text(encoding="utf-8")
)
acl = json.loads(
    (
        root
        / "feed/luci-app-agent-router/root/usr/share/rpcd/acl.d/luci-app-agent-router.json"
    ).read_text(encoding="utf-8")
)

assert "PKG_NAME:=nexus-cloud-connector" in makefile
assert "PKG_RELEASE:=43" in makefile
for dependency in ("+agentd", "+agent-gw", "+agent-adapter", "+curl", "+jshn", "+openssl-util"):
    assert dependency in makefile
assert "option enabled '0'" in config
assert "option enrollment_url ''" in config
assert "option token_file '/etc/nexus-cloud/device.token'" in config
assert "option identity_mode 'managed'" in config
assert "option sync_interval_seconds '120'" in config
assert "option recovery_interval_seconds '30'" in config
assert "option lease_seconds '300'" in config
assert "option connectivity_mode 'auto'" in config
assert "procd_add_reload_trigger nexus_cloud" in init_script
assert "procd_add_reload_trigger nexus_cloud agent" not in init_script
assert "procd_set_param file /etc/config/nexus_cloud" in init_script
assert "procd_set_param file /etc/config/nexus_cloud /etc/config/agent" not in init_script
assert 'procd_add_interface_trigger "interface.*"' not in init_script
assert 'umask 077' in daemon
assert "openssl base64 -A" in daemon
assert "openssl base64 -A -d" in daemon
assert "| base64" not in daemon
assert 'chmod 0600 "$temporary"' in daemon
assert "uci -q delete nexus_cloud.main.pairing_code" in daemon
assert 'if [ -n "$pairing_code" ] && [ -s "$token_file" ]' in daemon
assert "explicit pairing code requested Cloud identity replacement" in daemon
assert 'if [ "$REENROLLING" -eq 1 ]' in daemon
assert 'for cloud_url in "$base_url" "$enrollment_url"' in daemon
assert 'case "$cloud_url" in https://*)' in daemon
assert 'agent_gateway.main.cloud_public_origin="$enrollment_url"' in daemon
assert 'agent_gateway.main.cloud_trust_ca_file="$ca_file"' in daemon
assert 'ubus call agent agents' in daemon
assert 'ubus call agent addresses' in daemon
assert 'ubus call agent relay' in daemon and 'ubus call agent stats' in daemon
assert '/etc/init.d/agentd restart' not in daemon
assert 'agent.main.relay_config_generation' in daemon
assert 'Relay configuration generation unchanged' in daemon
assert 'cmp -s - /etc/agentd/nexus-cloud-forwarding.pem' in daemon
assert 'cmp -s - /etc/agentd/nexus-cloud-relay-ca.pem' in daemon
assert daemon.index('Relay configuration generation unchanged') < daemon.index('	mkdir -p /etc/agentd || {', daemon.index('Relay configuration generation unchanged'))
assert 'json_get_var applied_generation applied_generation' in daemon
assert "config.change" in daemon and '\"package\":\"agent\"' in daemon
assert 'agent_adapter.main.default_tenant="$value"' in daemon
assert '\"package\":\"agent_adapter\"' in daemon
assert 'client_certificate_fingerprint' in daemon
assert 'device_token_fingerprint' in daemon
assert 'directory_ca_fingerprint' in daemon
assert 'while [ "$attempt" -lt 60 ]' in daemon
assert 'NEXUS_RELAY_CONFIG_GENERATION' in agentd_init
assert '"applied_generation"' in agentd
assert 'procd_append_param env NEXUS_RELAY_CONFIG_GENERATION' in agentd_init
assert 'procd_append_param env NEXUS_RELAY_DEVICE_TOKEN_FILE' in agentd_init
assert 'procd_append_param env NEXUS_RELAY_DIRECTORY_CA_FILE' in agentd_init
assert 'case "$cloud_transport" in auto|direct_ipv6|relay)' in daemon
assert 'json_add_string transport "$transport"' in daemon
assert 'json_add_string relay_assignment_id "$active_assignment_id"' in daemon
assert "configure_relay_directory" in daemon
assert "nexus-cloud-forwarding.pem" in daemon and "nexus-cloud-relay-ca.pem" in daemon
assert "json_get_var source_router_id source_router_id" in daemon
assert 'agent.main.forwarding_source_router_id="$source_router_id"' in daemon
assert "forwarding_source_router_id" in agentd
assert 'procd_append_param command -9 "$forwarding_source_router_id"' in agentd_init
assert "expected.source_router_id = manager->forwarding_source_router_id;" in relay_invoke
assert "expected.source_router_id = manager->forwarding_verifier.issuer;" not in relay_invoke
assert "relay_directory_device_token_file" in daemon
assert 'agent.main.relay_directory_ca_file="$ca_file"' in daemon
assert 'agent.main.peer_ca_file=/etc/agentd/nexus-cloud-relay-ca.pem' in daemon
assert daemon.index('case "$endpoint" in') < daemon.index('relay_generation="$(')
assert "/api/v1/edge/v1/relay-configuration/" in daemon
assert "refresh_relay_configuration" in daemon
assert "disable_relay_configuration" in daemon
assert 'if [ "$cloud_transport" = direct_ipv6 ]; then\n\t\tdisable_relay_configuration' in daemon
disable_relay = daemon[
    daemon.index("disable_relay_configuration()") : daemon.index("certificate_thumbprint()")
]
assert "agent.main.relay_enabled=0" in disable_relay
assert "agent.main.relay_tunnel_enabled=0" in disable_relay
assert "agent.main.relay_gateway_internal_enabled=0" in disable_relay
assert "agent.main.forwarding_assertion_required=0" in disable_relay
assert "agent_gateway.main.internal_invoke_enabled=0" in disable_relay
assert "relay_directory_endpoints relay_directory_connect_ipv4s relay_config_generation" in disable_relay
assert '"package":"agent"' in disable_relay
assert '"package":"agent_gateway"' in disable_relay
assert "RELAY_NOT_CONFIGURED" in daemon and "relay_not_configured" in daemon
assert 'json_get_var relay_available available' in daemon
# A bodyless request must not inherit the false status of the optional
# data-binary test; otherwise GET, DELETE and empty POST return before curl.
assert "[ -n \"$payload\" ] && printf 'data-binary" in daemon
assert "\n\t\t:\n\t} >\"$curl_config\" || return 1" in daemon
assert "Relay configuration rejected at json" in daemon
assert "/etc/init.d/agentd reload" not in daemon
assert "/etc/init.d/agentd restart" not in daemon
assert 'old_agentd_pids="$(pidof agentd' not in daemon
assert "ubus call agent relay" in daemon
assert '"$applied_generation" = "$relay_generation"' in daemon
assert "agentd did not apply the desired generation" in daemon
assert "Relay configuration generation applied" in daemon
assert "add_mcp_tools" in daemon and "input_schema_json" in daemon
assert "computer-requirement" in daemon and "computer-scopes" in daemon
assert "json_add_object computer" in daemon
assert "json_select computer >/dev/null 2>&1" in daemon
for scope in ("files.list", "files.read", "files.write", "command.execute", "browser.control"):
    assert scope in daemon
assert "mobile-requirement" in daemon and "mobile-scopes" in daemon
assert "json_add_object mobile" in daemon
assert "json_select mobile >/dev/null 2>&1" in daemon
assert "caller_mobile_v1" in daemon
for scope in (
    "mobile.observe",
    "mobile.screen.capture",
    "mobile.tap",
    "mobile.type_text",
    "mobile.swipe",
    "mobile.press_back",
    "mobile.open_app",
    "mobile.wait_for_state",
):
    assert scope in daemon
assert '"$enrollment_url/api/v1/edge/nodes/enroll/"' in daemon
assert 'json_data_value "$response" edge_base_url' in daemon
assert 'uci set nexus_cloud.main.enrollment_url="$enrollment_url"' in daemon
assert 'uci set nexus_cloud.main.base_url="$edge_base_url"' in daemon
assert "openssl genpkey -algorithm EC" in daemon and "device_csr" in daemon
assert '"$base_url/api/v1/edge/v1/device-certificate/renew/"' in daemon
assert '"$base_url/api/v1/edge/v1/device-certificate/confirm/"' in daemon
assert "openssl verify -purpose sslclient" in daemon and "certificate_key_matches" in daemon
assert '"$base_url/api/v1/edge/v1/ingress-certificate/renew/"' in daemon
assert "prepare_managed_ingress_identity" in daemon and "install_managed_ingress_identity" in daemon
assert "openssl verify -purpose sslserver" in daemon and "openssl x509 -checkhost" in daemon
assert "direct_ingress_ready" in daemon and "ingress_listener_ready" in daemon
assert "apply_ingress_configuration" in daemon
assert '{"type":"config.change","data":{"package":"agent_gateway"}}' in daemon
assert "/etc/init.d/agent-gw restart" not in daemon
assert "/etc/init.d/agent-edge restart" not in daemon
assert 'uci set agent_gateway.main.mtls_server_cert="$MANAGED_INGRESS_CERT"' in daemon
assert "FORCE_MANAGED_INGRESS_RENEWAL=1" in daemon
assert "managed_ingress_identity_matches_node()" in daemon
managed_ingress_start = daemon.index("managed_ingress_identity_matches_node()")
managed_ingress_end = daemon.index("direct_ingress_ready()", managed_ingress_start)
managed_ingress_identity = daemon[managed_ingress_start:managed_ingress_end]
assert '[ -s "$node_id_file" ] && [ ! -L "$node_id_file" ]' in managed_ingress_identity
assert 'expected="edge-${normalized}.router.nexus"' in managed_ingress_identity
assert 'openssl x509 -checkhost "$expected"' in managed_ingress_identity
renew_ingress_start = daemon.index("renew_managed_ingress_identity()")
renew_ingress_end = daemon.index("enroll_node()", renew_ingress_start)
renew_ingress = daemon[renew_ingress_start:renew_ingress_end]
assert '[ "$FORCE_MANAGED_INGRESS_RENEWAL" -eq 0 ]' in renew_ingress
assert "managed_ingress_identity_matches_node" in renew_ingress
assert renew_ingress.index('install_managed_ingress_identity "$response"') < renew_ingress.index("FORCE_MANAGED_INGRESS_RENEWAL=0")
assert 'if direct_ingress_ready; then' in daemon
assert 'case "$public_transport" in\n\t\tmtls) direct_ready=1' not in daemon
assert '"$base_url/api/v1/edge/v1/agent-registrations/"' in daemon
assert 'curl_request DELETE' in daemon
assert "router_presence_v1" in daemon
assert '"$base_url/api/v1/edge/v1/presence/"' in daemon
assert "renew_presence()" in daemon and "withdraw_presence()" in daemon
assert "cloud_authorization_required()" in daemon
assert "prepare_cloud_runtime()" in daemon
assert "STATUS_RECOVERY_ACTION" in daemon
assert 'json_add_string recovery_action "$STATUS_RECOVERY_ACTION"' in daemon
assert 'json_add_int next_retry_seconds "${STATUS_NEXT_RETRY_SECONDS:-0}"' in daemon
assert 'case "$STATUS_RECOVERY_ACTION" in' in daemon
assert "reauthorization_required" in daemon
assert "automatic_retry" in daemon
assert "awaiting_agent_republish" in daemon
assert "AGENT_CLOUD_MANIFEST_MISSING" in daemon
assert 'json_add_int registration_http_code "${STATUS_REGISTRATION_HTTP_CODE:-0}"' in daemon
assert 'IFS="$CLOUD_STATUS_SEPARATOR" read -r origin state registration_id agent_id runtime_id transport mcp_url digest message64' in daemon
assert 'sleep "$retry_delay" &' in daemon
assert "STATUS_CLOUD_RELAY_AVAILABLE" in daemon
assert 'json_add_boolean cloud_relay_available "$STATUS_CLOUD_RELAY_AVAILABLE"' in daemon
assert 'json_add_boolean relay_available "$STATUS_CLOUD_RELAY_AVAILABLE"' in daemon
assert 'json_add_boolean relay_enabled "${STATUS_LOCAL_RELAY_ENABLED:-0}"' in daemon
assert 'json_add_boolean relay_ready "${STATUS_RELAY_READY:-0}"' in daemon
assert 'json_add_string snapshot_error_stage "$SNAPSHOT_ERROR_STAGE"' in daemon
disable_relay = daemon[daemon.index("disable_relay_configuration()"):daemon.index("certificate_thumbprint()")]
assert "STATUS_LOCAL_RELAY_ENABLED=0" in disable_relay
assert "STATUS_CLOUD_RELAY_AVAILABLE=0" not in disable_relay
probe_relay = daemon[daemon.index("probe_cloud_relay_availability()"):daemon.index("refresh_relay_configuration()")]
assert "/api/v1/edge/v1/relay-configuration/" in probe_relay
assert "STATUS_CLOUD_RELAY_AVAILABLE=1" in probe_relay
for snapshot_stage in ("agent_list", "address_list", "address_document", "agent_document", "manifest_document"):
    assert f"SNAPSHOT_ERROR_STAGE={snapshot_stage}" in daemon
presence_block = daemon[daemon.index("renew_presence()"):daemon.index("withdraw_presence()")]
assert "caller_mobile_v1" in presence_block and "runtime_context_v1" in presence_block
assert "durable_recovery_v1" in presence_block
assert "input_modalities64" in daemon
assert 'json_select input_modalities' in daemon
assert 'json_add_array input_modalities' in daemon
assert 'b64_encode "$input_modalities"' in daemon
assert 'json_get_var continuable continuable' in daemon
assert 'json_get_var continuable resumable' in daemon
assert 'json_get_var recovery_protocol recovery_protocol' in daemon
assert '[ -n "$recovery_protocol" ] || recovery_protocol=0' in daemon
assert 'json_add_boolean continuable "$continuable"' in daemon
assert 'json_add_int recovery_protocol "$recovery_protocol"' in daemon
stop_start = daemon.index("stop_connector()")
stop_end = daemon.index("main()", stop_start)
stop_block = daemon[stop_start:stop_end]
assert stop_block.index("withdraw_all") < stop_block.index("withdraw_presence")
main_block = daemon[daemon.index("main()"):]
assert "renew_presence degraded" in main_block
assert 'renew_presence "${SYNC_PRESENCE_STATE:-online}"' in main_block
assert "next_generation" in daemon and "GENERATION_FILE=/etc/nexus-cloud/generation" in daemon
assert "select_public_route" in daemon
assert "mcp_path_for_origin" in daemon
assert "od -An -v -tx1" not in daemon
assert "\\\\x$byte" not in daemon
assert "printf '%b'" not in daemon
assert 'character="${remaining%"${remaining#?}"}"' in daemon
assert "[A-Za-z0-9._~-]" in daemon
assert "printf '%02X'" in daemon
assert "origin_mcp_path_encoding_failed" in daemon
assert 'json_add_string path "$public_path"' in daemon
assert "origin_mcp_path_exceeds_255_bytes" in daemon
assert "configure_edge_auth" in daemon
assert "synchronize_gateway_cloud_trust" in daemon
assert "jwt_jwks_url" in daemon and "jwt_audience" in daemon and "jwt_issuer" in daemon
auth_start = daemon.index("configure_edge_auth()")
auth_end = daemon.index("configure_relay_directory()")
auth = daemon[auth_start:auth_end]
assert auth.index("/etc/init.d/agent-jwks stop") < auth.index("/usr/sbin/agent-jwks-update")
assert auth.index("/usr/sbin/agent-jwks-update") < auth.index("/etc/init.d/agent-jwks start")
assert "/usr/sbin/agent-jwks-update || return 1" in auth
assert "/etc/init.d/agent-jwks restart" not in auth
trust_start = daemon.index("synchronize_gateway_cloud_trust()")
trust_end = daemon.index("configure_relay_directory()", trust_start)
trust = daemon[trust_start:trust_end]
assert '[ -f "$ca_file" ] && [ ! -L "$ca_file" ]' in trust
assert 'agent_gateway.main.cloud_public_origin="$enrollment_url"' in trust
assert 'agent_gateway.main.cloud_trust_ca_file="$ca_file"' in trust
assert '{"type":"config.change","data":{"package":"agent_gateway"}}' in trust
enroll_start = daemon.index("enroll_node()")
enroll_end = daemon.index("next_generation()")
enroll = daemon[enroll_start:enroll_end]
persist_token = enroll.index('printf \'%s\\n\' "$device_token"')
clear_pairing = enroll.index("uci -q delete nexus_cloud.main.pairing_code")
apply_auth = enroll.index('configure_edge_auth "$response"')
apply_relay = enroll.index('configure_relay_directory "$response"')
assert persist_token < clear_pairing < apply_auth < apply_relay
prepare_start = daemon.index("prepare_cloud_runtime()")
prepare = daemon[prepare_start:daemon.index("main()", prepare_start)]
enrollment_gate = prepare.index('if [ "$REENROLLING" -eq 1 ] || [ ! -s "$token_file" ]')
trust_sync = prepare.index("synchronize_gateway_cloud_trust", enrollment_gate)
identity_renewal = prepare.index("renew_managed_identity", enrollment_gate)
assert enrollment_gate < trust_sync < identity_renewal
main = daemon[daemon.index("main()"):]
assert "if prepare_cloud_runtime; then" in main
assert 'minimal_config="${MINIMAL_CONFIG:-$project_dir/sdk/p8123-minimal.config}"' in build_script
assert "./scripts/feeds update luci" in build_script
assert "./scripts/feeds install -p luci luci-base" in build_script
assert "nexus-cloud-connector luci-app-agent-router" in build_script
assert 'cp "$minimal_config" .config' in build_script
assert "package/agent-cardd/compile package/luci-app-agent-router/compile" in build_script
assert "device_token" not in cloud_ui
assert "recoverySummary" in cloud_ui
assert "Automatic retry" in cloud_ui and "Pair again" in cloud_ui
assert "cloudRelayAvailability" in cloud_ui
assert "cloud_relay_available" in cloud_ui
assert "relay_enabled" in cloud_ui and "relay_ready" in cloud_ui
assert "snapshot_error_stage" in cloud_ui
assert "'require poll';" in cloud_ui and "'require dom';" in cloud_ui
assert "poll.add" in cloud_ui and "dom.content" in cloud_ui
assert "value === 'relay' && relayAvailable === false" not in cloud_ui
assert menu["admin/status/agent-router/developer/cloud"]["action"]["path"] == "agent-router/cloud"
assert menu["admin/status/agent-router/cloud"]["action"]["path"] == "admin/status/agent-router/home"
configure_acl = acl["luci-app-agent-router-configure"]
assert "nexus_cloud" in configure_acl["read"]["uci"]
assert "nexus_cloud" in configure_acl["write"]["uci"]
assert configure_acl["read"]["file"]["/var/run/nexus-cloud/status.json"] == ["read"]
assert configure_acl["read"]["ubus"]["nexus-cloud"] == ["check"]
assert '"check":{}' in rpcd
for check in ("Cloud TLS", "Certificate and key", "JWT keys", "Client authentication"):
    assert check in tls_check
for check in ("Cloud registration", "Cloud identity files", "Cloud identity", "Cloud Relay"):
    assert check in tls_check
assert "/api/v1/edge/v1/relay-configuration/" in tls_check
assert 'header = "Authorization: Edge %s"' in tls_check
assert 'rm -f "$probe_config" "$probe_response"' in tls_check
assert "RELAY_NOT_CONFIGURED" in tls_check
assert "file_mode()" in tls_check
assert "command -v stat" in tls_check
assert "-rw-------" in tls_check

print("Nexus Cloud connector contract passed")

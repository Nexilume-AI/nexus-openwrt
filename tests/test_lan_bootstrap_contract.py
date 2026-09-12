from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_bootstrap_is_routed_only_by_the_lan_bridge() -> None:
    proxy = _text("feed/common/agent_edge_proxy.c")
    public = proxy.split("static enum agent_edge_backend public_request_backend", 1)[1]
    public = public.split("static bool exact_path", 1)[0]
    lan = proxy.split("static enum agent_edge_backend lan_request_backend", 1)[1]
    lan = lan.split("static enum agent_edge_backend rewrite_http", 1)[0]

    assert "/agent/v1/bootstrap" not in public
    assert 'exact_path(path, path_length, "/agent/v1/bootstrap")' in lan
    assert "/agent/v1/cloud-registration" not in public
    assert 'exact_path(path, path_length, "/agent/v1/cloud-registration")' in lan
    assert "AGENT_LAN_SOURCE_HEADER" in proxy
    assert "AGENT_LAN_BRIDGE_TOKEN_HEADER" in proxy


def test_gateway_hides_bootstrap_without_authenticated_lan_metadata() -> None:
    gateway = _text("feed/agent-gw/src/agent_gateway.c")
    bootstrap = gateway.split("static void bootstrap_handler", 1)[1]
    bootstrap = bootstrap.split("static void authentication_metadata_handler", 1)[0]

    assert '#include "agent_edge_proxy.h"' in gateway
    assert "event == UH_EV_HEAD_COMPLETE" in bootstrap
    assert "connection->check_expect_100_continue(connection)" in bootstrap
    assert "body.len == 0U || body.len > 4096U" in bootstrap
    assert "session_token[token_length] = '\\0'" in gateway
    assert "memset(session_token, 0, sizeof(session_token))" in gateway
    assert 'json_object_new_int(trusted_lan ? 2 : 1)' in gateway
    assert 'json_object_new_string("trusted-lan")' in gateway
    assert 'json_object_new_string("/agent/v1/bootstrap")' in gateway
    assert 'send_error(connection, 404, "NOT_FOUND", "endpoint not found")' in gateway
    assert 'state->lan_identity_mismatch ? 401' in gateway
    assert '"lan_bootstrap_refreshed"' in gateway


def test_successful_lan_bootstrap_delivers_bounded_cloud_trust() -> None:
    gateway = _text("feed/agent-gw/src/agent_gateway.c")
    connector = _text(
        "feed/nexus-cloud-connector/files/nexus-cloud-connectord"
    )
    gateway_init = _text("feed/agent-gw/files/agent-gw.init")

    bootstrap = gateway.split("static void bootstrap_handler", 1)[1]
    bootstrap = bootstrap.split("static void authentication_metadata_handler", 1)[0]
    assert 'json_object_new_int(2)' in bootstrap
    assert '"cloud_trust"' in bootstrap
    assert '"pinned-pem"' in bootstrap and '"system"' in bootstrap
    assert '"ca_pem"' in bootstrap and '"sha256"' in bootstrap
    assert "GATEWAY_CLOUD_CA_MAX 65536U" in gateway
    assert "cloud_ca_pem_is_certificate_only" in gateway
    assert 'strstr(config.cloud_trust_ca_pem, "PRIVATE KEY")' not in gateway
    assert "O_NOFOLLOW" in gateway and "S_ISREG" in gateway
    assert '"bootstrap-response"' in gateway
    assert 'agent_gateway.main.cloud_public_origin="$base_url"' in connector
    assert 'agent_gateway.main.cloud_trust_ca_file="$ca_file"' in connector
    assert 'procd_append_param command -4 "$cloud_public_origin"' in gateway_init
    assert 'procd_append_param command -5 "$cloud_trust_ca_file"' in gateway_init
    assert 'procd_add_jail_mount "$cloud_trust_ca_file"' in gateway_init


def test_lan_sdk_switch_drives_runtime_only_rotating_secrets() -> None:
    gateway_init = _text("feed/agent-gw/files/agent-gw.init")
    edge_init = _text("feed/agent-gw/files/agent-edge.init")

    assert "config_get_bool lan_sdk_enabled main lan_sdk_enabled 0" in gateway_init
    assert 'lan_session_key_file=/var/run/agent-gw/lan-session.key' in gateway_init
    assert 'generate_runtime_secret "$lan_session_key_file.tmp.$$"' not in gateway_init
    assert 'secret_tmp="$lan_session_key_file.tmp.$$"' in gateway_init
    assert 'generate_runtime_secret "$secret_tmp"' in gateway_init
    assert 'procd_append_param command -0 "$lan_sdk_enabled"' in gateway_init
    assert 'procd_append_param command -T "$lan_bridge_token_file"' in edge_init

import pathlib
import sys


root = pathlib.Path(sys.argv[1]).resolve()
gateway = (root / "feed/agent-gw/src/agent_gateway.c").read_text(encoding="utf-8")
config = (root / "feed/agent-gw/files/agent-gw.config").read_text(encoding="utf-8")
init = (root / "feed/agent-gw/files/agent-gw.init").read_text(encoding="utf-8")
protocol = (root / "feed/common/agent_ipc_protocol.h").read_text(encoding="utf-8")
server = (root / "feed/agentd/src/agent_ipc_server.c").read_text(encoding="utf-8")
daemon = "\n".join(
    (root / path).read_text(encoding="utf-8")
    for path in ("feed/agentd/src/agentd.c", "feed/agentd/src/agent_relay_invoke.c")
)
agentd_makefile = (root / "feed/agentd/Makefile").read_text(encoding="utf-8")
gateway_makefile = (root / "feed/agent-gw/Makefile").read_text(encoding="utf-8")

for path in ("register", "renew", "unregister"):
    assert f'"^/agent/v1/{path}$"' in gateway
assert '"agent.register"' in gateway
assert "registration_enabled" in gateway
assert "option registration_enabled '0'" in config
assert 'command -g "$registration_enabled"' in init
for field in (
    "public_descriptor_enabled",
    "public_tls_server_name",
    "public_ca_bundle_id",
    "public_ingress_port",
    "public_scheme",
    "public_endpoint",
):
    assert field in gateway
assert "option public_descriptor_enabled '0'" in config
assert 'command -D "$public_descriptor_enabled"' in init
assert 'command -H "$public_tls_server_name"' in init
assert 'command -C "$public_ca_bundle_id"' in init
assert "#define AGENT_IPC_VERSION 13U" in protocol
assert "#define AGENT_IPC_MIN_VERSION 8U" in protocol
assert "request_public_ipv6" in protocol
assert '"public_ipv6"' in gateway
assert "target_agent" in protocol
assert '"target_agent"' in gateway
assert "query->target_agent" in server
assert "target_agent_mismatch" in daemon
assert "PKG_VERSION:=3.1.0" in agentd_makefile
assert "PKG_VERSION:=0.22.0" in gateway_makefile
assert "agent_invoke_parse_lan_endpoint" in gateway
assert "agent_invoke_parse_lan_endpoint" in daemon
assert "lan_backend_enabled" in gateway
assert "option lan_backend_enabled '1'" in config
assert 'command -M "$lan_backend_enabled"' in init
assert '"^/agent/v1/authentication$"' in gateway
assert 'memcmp(path.p, metadata_path' in gateway
assert "authentication_metadata_handler" in gateway
assert '"required_scopes"' in gateway
for message in (
    "AGENT_IPC_REGISTER_REQUEST",
    "AGENT_IPC_RENEW_REQUEST",
    "AGENT_IPC_UNREGISTER_REQUEST",
    "AGENT_IPC_LEASE_RESPONSE",
):
    assert message in protocol
for message in (
    "AGENT_IPC_REGISTER_REQUEST",
    "AGENT_IPC_RENEW_REQUEST",
    "AGENT_IPC_UNREGISTER_REQUEST",
):
    assert message in server
assert "agent_ipc_encode_lease_response" in server
assert "ipc_register_local_route" in daemon
assert "broadcast_route_update" in daemon
assert "ipc_unregister_local_route" in daemon
assert "broadcast_route_withdraw" in daemon
assert "requester_agent" in protocol
assert "route lease belongs to another Agent" in daemon

print("Agent Access Proxy registration contract passed")

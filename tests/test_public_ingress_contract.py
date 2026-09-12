import pathlib
import sys


root = pathlib.Path(sys.argv[1]).resolve()
protocol = (root / "feed/common/agent_ipc_protocol.h").read_text(encoding="utf-8")
server = (root / "feed/agentd/src/agent_ipc_server.c").read_text(encoding="utf-8")
routes = (root / "feed/agentd/src/route_table.c").read_text(encoding="utf-8")
gateway = (root / "feed/agent-gw/src/agent_gateway.c").read_text(encoding="utf-8")
bridge = (root / "feed/agent-gw/src/agent_edge_bridge.c").read_text(encoding="utf-8")
edge_proxy = (root / "feed/common/agent_edge_proxy.c").read_text(encoding="utf-8")
edge = (root / "feed/agent-gw/files/agent-edge.init").read_text(encoding="utf-8")
adapter = (root / "feed/agent-adapter/src/agent_adapterd.c").read_text(encoding="utf-8")
adapter_codec = (root / "feed/agent-adapter/src/adapter_codec.c").read_text(encoding="utf-8")
adapter_registry = (root / "feed/agent-adapter/src/adapter_registry.c").read_text(encoding="utf-8")
adapter_registry_header = (root / "feed/agent-adapter/src/adapter_registry.h").read_text(encoding="utf-8")
adapter_contract = (root / "feed/common/agent_adapter_ingress_contract.c").read_text(encoding="utf-8")
gateway_makefile = (root / "feed/agent-gw/Makefile").read_text(encoding="utf-8")
agentd_makefile = (root / "feed/agentd/Makefile").read_text(encoding="utf-8")
adapter_makefile = (root / "feed/agent-adapter/Makefile").read_text(encoding="utf-8")

assert "#define AGENT_IPC_VERSION 13U" in protocol
assert "#define AGENT_IPC_MIN_VERSION 8U" in protocol
assert "char public_ipv6[AGENT_IPC_IPV6_LEN]" in protocol
assert "agent_public_ipv6_find_address" in server
lookup_selection = server.split("static bool lookup_selection(", 1)[1].split(
    "static void handle_single_lookup", 1
)[0]
assert "route_table_lookup_id" not in lookup_selection
assert "route_table_lookup(server->routes, query" in lookup_selection
assert "query->target_agent[0] == '\\0'" in lookup_selection
assert "strcmp(query->target_agent, lease->origin) != 0" in lookup_selection
assert "selection->route->source != AGENT_ROUTE_SOURCE_LOCAL" in lookup_selection
assert "strcmp(selection->route->origin, lease->origin) != 0" in lookup_selection
assert "bool route_table_lookup_id(" in routes
assert "capture_public_ingress" in gateway
assert "PUBLIC_ROUTE_PINNED" in gateway
assert "state->request.public_ipv6" in gateway
assert "public_ingress_route_misses" in gateway
assert "last_public_ipv6" in gateway
assert "local_dynamic" in adapter_registry_header
assert 'local_dynamic ? "local" : options->region' in adapter_codec
assert "effective->local_dynamic[current_index] = true" in adapter_registry
assert "agent_public_ingress_validate" in gateway
assert "agent_edge_proxy_parse_v1" in bridge
assert "config->direct_ipv6" in bridge
assert "getsockname" in bridge
assert "case 'r':" in bridge
assert "agent_public_ipv6_contains" in bridge
assert "info.destination_port != config->public_port" in bridge
assert "AGENT_EDGE_BACKEND_ADAPTER" in edge_proxy
assert 'echo "protocol = proxy"' in edge
assert "/usr/sbin/agent-edge-bridge" in edge
assert "AGENT_PUBLIC_INGRESS_DEST_HEADER" in adapter
assert "AGENT_PUBLIC_INGRESS_TOKEN_HEADER" in adapter
assert "credentials.public_ipv6" in adapter
assert "credentials.edge_token" in adapter
assert "AGENT_PUBLIC_INGRESS_DEST_HEADER" in adapter_contract
assert "PKG_VERSION:=3.1.0" in agentd_makefile
assert "PKG_VERSION:=0.22.0" in gateway_makefile
assert '"/agent/v1/authentication"' in edge_proxy
assert "agent_edge_proxy_rewrite_lan_http" in edge_proxy
assert "config->lan_mode" in bridge
assert "case 'l':" in bridge
assert "no_jwt_lan_enabled" in edge
assert "auth_mode=none" in edge
assert "PKG_VERSION:=0.6.0" in adapter_makefile
assert '"target_agent"' in adapter_codec
assert "TARGET_AGENT_MISMATCH" in gateway

print("P8.12.2 public /128 ingress binding contract passed")

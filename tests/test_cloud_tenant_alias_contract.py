from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_direct_cloud_alias_is_exact_and_fail_closed() -> None:
    gateway = _read("feed/agent-gw/src/agent_gateway.c")
    handler = gateway.split("static void gateway_request_handler", 1)[1]

    assert '"/var/run/nexus-agent-cloud/tenant-id"' in gateway
    assert '"/var/run/agent-manifests/manifests.json"' in gateway
    assert "state->lan_session || !state->identity.verified" in gateway
    assert "state->authenticated_target_agent[0] == '\\0'" in gateway
    assert "strcmp(state->authenticated_target_agent," in gateway
    assert "strcmp(state->request.tenant, cloud_tenant) != 0" in gateway
    assert '!json_object_object_get_ex(item, "publish", &publish)' in gateway
    assert "!json_object_get_boolean(publish)" in gateway
    assert 'json_object_object_get_ex(item, "origin", &origin)' in gateway
    assert 'json_object_object_get_ex(tool, "authority", &authority)' in gateway
    assert 'json_object_object_get_ex(tool, "intent", &intent)' in gateway
    assert handler.index("TARGET_AGENT_MISMATCH") < handler.index(
        "apply_cloud_tenant_alias(state)"
    ) < handler.index("start_async_ipc(state")


def test_interactive_timeout_requires_adapter_and_manifest_policy() -> None:
    gateway = _read("feed/agent-gw/src/agent_gateway.c")
    gateway_init = _read("feed/agent-gw/files/agent-gw.init")
    adapter = _read("feed/agent-adapter/src/agent_adapterd.c")
    adapter_codec = _read("feed/agent-adapter/src/adapter_codec.c")
    adapter_init = _read("feed/agent-adapter/files/agent-adapter.init")

    assert 'json_object_object_add(flags, "interactive"' in adapter_codec
    assert "mapping->task || mapping->interactive" in adapter_codec
    assert "state->normalized.interactive" in adapter
    assert "config.interactive_timeout_ms" in adapter
    assert 'command -I "$interactive_timeout"' in adapter_init
    assert "manifest_allows_interactive_timeout" in gateway
    assert "state->interactive_requested" in gateway
    assert 'json_object_object_get_ex(tool, "task", &task)' in gateway
    assert 'json_object_object_get_ex(tool, "chat", &chat)' in gateway
    assert 'json_object_object_get_ex(tool, "interactive", &interactive)' in gateway
    assert "config.interactive_backend_timeout_ms" in gateway
    assert 'command -3 "$interactive_backend_timeout"' in gateway_init
    handler = gateway.split("static void gateway_request_handler", 1)[1]
    assert handler.index("TARGET_AGENT_MISMATCH") < handler.index(
        "apply_interactive_backend_timeout(state)"
    ) < handler.index("start_async_ipc(state")


def test_relay_alias_requires_nfa_body_bound_target() -> None:
    relay = _read("feed/agentd/src/agent_relay_invoke.c")
    agentd = _read("feed/agentd/src/agentd.c")

    end_handler = relay.split("if (message->type == AGENT_RELAY_TUNNEL_END)", 1)[1]
    assert "agent_forwarding_verify_body(" in end_handler
    assert "signed_envelope_target_matches(slot)" in end_handler
    assert end_handler.index("agent_forwarding_verify_body(") < end_handler.index(
        "signed_envelope_target_matches(slot)"
    ) < end_handler.index("start_local_invoke(slot)")
    assert 'json_object_object_get_ex(root, "target_agent", &target)' in relay
    assert "slot->manager->forwarding_required" in relay
    assert "resolve_relay_cloud_tenant_alias" in agentd
    assert "strcmp(cloud_tenant, enrolled_tenant) != 0" in agentd
    assert "!entry->publish" in agentd
    assert "strcmp(entry->origin, target_agent) != 0" in agentd
    assert "strcmp(entry->mapping.intent, intent) != 0" in agentd


def test_connector_projects_cloud_tenant_without_sdk_credentials() -> None:
    connector = _read("feed/nexus-cloud-connector/files/nexus-cloud-connectord")
    gateway_init = _read("feed/agent-gw/files/agent-gw.init")
    agentd_init = _read("feed/agentd/files/agentd.init")

    assert "CLOUD_TENANT_ID_FILE=/etc/nexus-cloud/tenant.id" in connector
    assert "AGENT_CLOUD_TENANT_ID=$AGENT_CLOUD_STATUS_DIR/tenant-id" in connector
    assert 'tenant_id="$(json_data_value "$response" tenant_id)"' in connector
    assert 'agent_adapter.main.default_tenant="$value"' in connector
    assert '"package":"agent_adapter"' in connector
    assert '[ "$current" = "$value" ] && return 0' in connector
    assert 'chmod 0600 "$temporary"' in connector
    assert 'chmod 0644 "$temporary"' in connector
    assert 'rm -f "$AGENT_CLOUD_ENABLED" "$AGENT_CLOUD_ENROLLED" "$AGENT_CLOUD_TENANT_ID"' in connector
    assert "procd_add_jail_mount /var/run/agent-manifests" in gateway_init
    assert "procd_add_jail_mount /var/run/nexus-agent-cloud" in agentd_init


def test_connector_preserves_terminal_states_and_mapping_conflicts() -> None:
    connector = _read("feed/nexus-cloud-connector/files/nexus-cloud-connectord")

    assert "MCP_MAPPING_CONFLICT=1" in connector
    assert "MCP_STATIC_MAPPINGS" in connector
    assert "mapping_signature()" in connector
    assert 'LAST_ERROR_CODE=MAPPING_CONFLICT' in connector
    assert 'provisioning_state=rejected' in connector
    assert 'provisioning_state=degraded' in connector
    assert 'skip_reason=mapping_conflict' in connector
    assert 'registration_id="$(register_agent' not in connector
    assert 'if [ -z "$provisioning_state" ]; then' in connector
    assert connector.index('if [ -z "$provisioning_state" ]; then') < connector.index(
        "provisioning_state=ready"
    )


def test_dynamic_manifest_binds_one_local_tenant_per_origin() -> None:
    manifest_h = _read("feed/agentd/src/agent_dynamic_manifest.h")
    manifest_c = _read("feed/agentd/src/agent_dynamic_manifest.c")
    agentd = _read("feed/agentd/src/agentd.c")

    assert "char tenant[AGENT_TENANT_LEN];" in manifest_h
    assert "AGENT_DYNAMIC_MANIFEST_TENANT_CONFLICT" in manifest_h
    assert "strcmp(current->tenant, entry->tenant) != 0" in manifest_c
    assert 'json_object_object_add(item, "tenant"' in agentd
    assert "route.tenant" in agentd

#!/usr/bin/env python3
"""Cross-package security contract for Relay -> HTTPS SDK execution."""

from __future__ import annotations

import re
import sys
from pathlib import Path

if len(sys.argv) > 2:
    raise SystemExit("usage: test_relay_https_gateway_contract.py [ROOT]")

root = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


gateway = read("feed/agent-gw/src/agent_gateway.c")
gateway_init = read("feed/agent-gw/files/agent-gw.init")
gateway_uci = read("feed/agent-gw/files/agent-gw.config")
gateway_make = read("feed/agent-gw/Makefile")
adapter_make = read("feed/agent-adapter/Makefile")
edge_init = read("feed/agent-gw/files/agent-edge.init")
agentd = read("feed/agentd/src/agentd.c")
relay = read("feed/agentd/src/agent_relay_invoke.c")
agentd_init = read("feed/agentd/files/agentd.init")
agentd_uci = read("feed/agentd/files/agentd.config")
connector = read("feed/nexus-cloud-connector/files/nexus-cloud-connectord")
invoke = read("feed/common/agent_invoke_contract.c")
invoke_header = read("feed/common/agent_invoke_contract.h")
edge_test = read("tests/test_edge_proxy.c")
invoke_test = read("tests/test_invoke_contract.c")

raw_path = "/etc/agentd/nexus-cloud-gateway.token"
digest_path = "/etc/agent-gw/nexus-cloud-gateway-token.sha256"
internal_path = "/agent/v1/internal-invoke"

for value in (
    "openssl rand -hex 32",
    'chown root:root "$temporary"',
    'chmod 0600 "$temporary"',
    'mv -f "$temporary" "$GATEWAY_TOKEN_FILE"',
    'chown root:agentd "$digest_temporary"',
    'chmod 0640 "$digest_temporary"',
    'mv -f "$digest_temporary" "$GATEWAY_TOKEN_DIGEST_FILE"',
    '"$device_token_fingerprint" "$GATEWAY_TOKEN_FINGERPRINT"',
    "openssl rand -hex 16",
    'internal_invoke_generation="$gateway_apply_generation"',
    'json_get_var gateway_loaded_generation internal_invoke_generation',
    '"1:$gateway_apply_generation"|"true:$gateway_apply_generation"',
):
    require(value in connector, f"connector lifecycle missing: {value}")
require(raw_path in connector and digest_path in connector, "managed paths missing")
require(
    connector.index("internal Gateway did not become ready")
    < connector.index('\"type\":\"config.change\",\"data\":{\"package\":\"agent\"}'),
    "agentd must reload only after Gateway readiness",
)

require(raw_path not in gateway, "Gateway must not reference the raw token")
require(raw_path not in gateway_init and raw_path not in gateway_uci,
        "Gateway service must not mount the raw token")
digest_reader = gateway[
    gateway.index("static bool read_internal_invoke_token_digest"):
    gateway.index("static bool internal_invoke_token_matches_digest")
]
for value in (
    "O_RDONLY | O_CLOEXEC | O_NOFOLLOW",
    "metadata.st_uid != 0U",
    "(metadata.st_mode & 0777U) != 0640U",
    "!S_ISREG(metadata.st_mode)",
):
    require(value in digest_reader,
            f"Gateway verifier reader permission check missing: {value}")
for value in (
    "mbedtls_sha256",
    "internal_invoke_token_matches_digest",
    "agent_invoke_internal_token_matches",
    "agent_invoke_internal_selection_validate",
    "AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH",
    "internal_dynamic_map_pinned",
    "dynamic_map = &state->internal_dynamic_map",
    "remote_map = &dynamic_map->mapping",
    "lease_remaining_ms",
    "start_async_backend(state",
    "internal_invoke_auth_rejections",
    "internal_invoke_route_rejections",
    '"internal_invoke_generation"',
    "agent_invoke_internal_generation_is_valid",
    "agent_invoke_backend_response_limit",
    "state->effective_max_backend_response_bytes",
    "state->registration_tls_endpoint = parsed;",
    "state->operation == GATEWAY_OPERATION_REGISTER &&",
):
    require(value in gateway, f"Gateway fail-closed path missing: {value}")
handler = gateway[gateway.index("static void internal_invoke_handler"):gateway.index("static void gateway_request_handler")]
require("start_async_ipc(state" not in handler, "internal invoke must not use AFIB IPC")
require("try_next_route_candidate" not in handler, "internal invoke must not retry")
require("&state->internal_dynamic_map" in handler,
        "internal invoke must copy an immutable dynamic-lease snapshot")
for value in (
    "internal_timeout_ms <\n        config.backend_timeout_ms",
    "lease_remaining_ms < state->effective_backend_timeout_ms",
    "state->backend_deadline_ms = now_ms +",
):
    require(value in handler,
            f"internal invoke deadline budget is not bounded: {value}")
backend = gateway[gateway.index("static bool start_async_backend(\n", gateway.index("static bool start_async_backend(\n") + 1):gateway.index("static void gateway_request_handler")]
internal_backend = backend[
    backend.index("if (state->internal_invoke) {"):
    backend.index("} else {", backend.index("if (state->internal_invoke) {"))
]
require("find_remote_map" not in internal_backend and
        "config.remote_maps" not in internal_backend,
        "internal invoke must never fall back to mutable/static route lookup")
require("config.internal_invoke_enabled = false;" in gateway,
        "internal endpoint must default disabled")
require("-Z internal_invoke_generation" in gateway and
        'case \'Z\':' in gateway and
        'command -Z "$internal_invoke_generation"' in gateway_init,
        "Gateway rollout generation must cross UCI/procd/argv boundaries")
require("state->effective_max_backend_response_bytes" in gateway and
        "agent_invoke_backend_response_limit(" in gateway and
        "state->effective_max_backend_response_bytes," in gateway,
        "internal backend parser must use the effective 16 KiB bound")
require("AGENT_IPC_MAX_INVOKE_BODY" in invoke and
        "agent_invoke_backend_response_limit(true, 262144U)" in invoke_test,
        "the internal 16 KiB response clamp needs executable coverage")
adapter_release = re.search(r"^PKG_RELEASE:=(\d+)$", adapter_make, re.MULTILINE)
require(adapter_release is not None and int(adapter_release.group(1)) >= 8 and
        "../common/agent_ipc_protocol.h" in adapter_make,
        "agent-adapter must package the public IPC bound used by the invoke contract")

for value in (
    "load_internal_gateway_token",
    "(metadata.st_mode & 0777U) != 0600U",
    "slot->open.target_agent[0] == '\\0'",
    "gateway_timeout_ms = (uint32_t)remaining",
    "agent_invoke_build_internal_http_request",
    "slot->internal_gateway = true",
    "internal_gateway_completed++",
    "erase_secret(manager->internal_gateway_token",
):
    require(value in relay, f"agentd delegation missing: {value}")
require("!config.forwarding_assertion_required" in agentd,
        "agentd bridge must require NFA")
require("config.relay_gateway_internal_enabled = false;" in agentd,
        "agentd bridge must default disabled")
require(raw_path in agentd_init and raw_path in agentd_uci,
        "agentd must receive the raw managed credential")

for value in (
    "AGENT_INVOKE_INTERNAL_TIMEOUT_HEADER",
    '"%s: %u\\r\\n%s\\r\\n"',
    "agent_invoke_internal_selection_validate",
    "agent_invoke_dynamic_map_find",
    "target->endpoint = *endpoint",
    "endpoint_matches(&entry->endpoint, endpoint)",
):
    require(value in invoke or value in invoke_header,
            f"common contract missing: {value}")
require("https://probe.edge.test:9443/other" in invoke_test,
        "dynamic lease must reject a different path on the same TLS origin")

require("agent-edge=459:agent-edge=459" in gateway_make,
        "agent-edge must use dedicated UID/GID 459")
require(edge_init.count("procd_set_param user agent-edge") == 3,
        "all bridge instances must use agent-edge")
require(edge_init.count("procd_set_param group agentd") == 3,
        "edge instances must retain the shared non-secret agentd group")
require('echo "setuid = agent-edge"' in edge_init,
        "stunnel must drop to agent-edge")
require('echo "setgid = agentd"' in edge_init,
        "stunnel must retain the shared non-secret agentd group")
require("TIMEOUTidle" not in edge_init,
        "stunnel must not override application-layer interactive deadlines")
require(edge_test.count(internal_path) >= 2,
        "public and LAN bridges must reject internal invoke")

uid_owners: dict[int, str] = {}
for makefile in (root / "feed").glob("*/Makefile"):
    for name, uid in re.findall(
        r"([a-z][a-z0-9-]*)=(\d+):",
        makefile.read_text(encoding="utf-8"),
    ):
        number = int(uid)
        require(number not in uid_owners,
                f"feed UID {number} shared by {uid_owners.get(number)} and {name}")
        uid_owners[number] = name
require(uid_owners.get(454) == "agent-gw" and uid_owners.get(459) == "agent-edge",
        "Gateway and edge UIDs must remain distinct")

health = gateway[gateway.index("static void health_handler"):gateway.index("static void authentication_metadata_handler")]
require('"internal_invoke_token' not in health and digest_path not in health,
        "Gateway health must not expose verifier material")
for field in (
    "last_auth_failure_stage",
    "last_auth_jwt_result",
    "last_auth_claim_result",
):
    require(f'"{field}"' in health,
            f"Gateway health is missing non-secret JWT diagnostic {field}")
require("agent_jwt_result_name(" in health and
        "agent_auth_result_name(" in health,
        "Gateway health must report bounded result names, not claims")
require('blobmsg_add_string(&response, "relay_gateway_token_file"' not in agentd,
        "agentd status must not expose the credential path")
require('blobmsg_add_string(&response, "relay_gateway_token"' not in agentd,
        "agentd status must not expose the raw credential")
require("manager->internal_gateway_token" not in "\n".join(
    line for line in relay.splitlines()
    if "printf" in line or "syslog" in line or "blobmsg" in line
), "agentd logs/status must not emit the raw credential")
require(not re.search(r"\blog\s+.*\$GATEWAY_TOKEN(?:_FILE)?\b", connector),
        "connector logs must not emit the raw credential or its path")

cloud_status = gateway[
    gateway.index("static void cloud_registration_handler"):
    gateway.index("static void not_found_handler")
]
require("if (event != UH_EV_COMPLETE) return;" in cloud_status,
        "Cloud status GET must respond in the bodyless COMPLETE callback")
require("if (event == UH_EV_HEAD_COMPLETE)" not in cloud_status,
        "Cloud status GET must not wait for a second body event")

require(int(re.search(r"^PKG_RELEASE:=(\d+)$", gateway_make, re.MULTILINE).group(1)) >= 25, "agent-gw release not advanced")
agentd_release = re.search(r"^PKG_RELEASE:=(\d+)$", read("feed/agentd/Makefile"), re.MULTILINE)
require(
    agentd_release is not None and int(agentd_release.group(1)) >= 18,
    "agentd release not advanced",
)
require(int(re.search(r"^PKG_RELEASE:=(\d+)$", read("feed/nexus-cloud-connector/Makefile"), re.MULTILINE).group(1)) >= 35,
        "connector release not advanced")

print("Relay HTTPS Gateway security contract passed")

#!/usr/bin/env python3
"""Static P7.3-P7.6 contract checks for luci-app-agent-router."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    package = root / "feed" / "luci-app-agent-router"
    views = package / "htdocs" / "luci-static" / "resources" / "view"
    resources = package / "htdocs" / "luci-static" / "resources"
    menu_path = package / "root" / "usr" / "share" / "luci" / "menu.d" / "luci-app-agent-router.json"
    acl_path = package / "root" / "usr" / "share" / "rpcd" / "acl.d" / "luci-app-agent-router.json"

    require(package.is_dir(), "LuCI package directory is missing")
    menu = json.loads(menu_path.read_text(encoding="utf-8"))
    acl = json.loads(acl_path.read_text(encoding="utf-8"))

    view_entries = [entry for entry in menu.values() if entry.get("action", {}).get("type") == "view"]
    require(len(view_entries) == 13, "LuCI must expose one user workbench and twelve developer views")
    for entry in view_entries:
        view_path = entry["action"]["path"]
        require((views / f"{view_path}.js").is_file(), f"menu view missing: {view_path}")

    read_methods = set(acl["luci-app-agent-router"]["read"]["ubus"]["agent"])
    allowed_read = {
        "stats", "recovery", "policy", "routes", "neighbors",
        "agents", "addresses", "discoveries", "cross_discoveries", "promotions", "cards", "card_trust",
        "relay", "open_mesh_relay",
    }
    require(read_methods == allowed_read, "read ACL differs from the bounded status API")
    require(acl["luci-app-agent-router"]["read"]["ubus"].get("nexus-agent-ui") == ["overview", "mesh_status"],
            "read-only users must only access the sanitized user overview")

    trust_acl = acl["luci-app-agent-router-trust"]
    require(set(trust_acl["read"]["ubus"]["agent"]) == {
        "discoveries", "cross_discoveries", "promotions", "cards", "card_trust",
    }, "Peer Trust read ACL is wider than its evidence inputs")
    require(set(trust_acl["write"]["ubus"]["agent"]) == {
        "discovery_promote", "discovery_revoke", "card_revoke",
    }, "Peer Trust write ACL is wider than confirm/revoke operations")

    write = acl["luci-app-agent-router-configure"]["write"]
    write_methods = set(write["ubus"]["agent"])
    allowed_write = {
        "reload", "reload_policy", "reload_peers", "discovery_refresh",
        "cross_discovery_refresh", "relay_refresh", "open_mesh_relay_refresh", "discovery_promote",
        "discovery_revoke", "card_revoke",
    }
    require(write_methods == allowed_write, "write ACL contains an unapproved agent mutation")
    require(write["ubus"].get("nexus-agent-ui") == ["set_feature", "pair_cloud", "refresh", "setup_mesh", "share_mesh", "set_directory_relays"],
            "configure ACL must expose only the bounded user-mode mutations")
    require(set(write["uci"]) == {
        "agent", "agent_peers", "agent_policy", "nexus_roles",
        "agent_adapter", "agent_gateway", "nexus_cloud",
    }, "unexpected UCI write scope")

    js_files = sorted(resources.rglob("*.js"))
    css_files = sorted(resources.rglob("*.css"))
    require(len(js_files) == 17, "unexpected JavaScript file count")
    source = "\n".join(path.read_text(encoding="utf-8") for path in js_files)
    identity_source = "\n".join(
        (resources / "view" / "agent-router" / name).read_text(encoding="utf-8")
        for name in ("setup.js", "settings.js", "peers.js")
    )
    styles = "\n".join(path.read_text(encoding="utf-8") for path in css_files)

    for forbidden in (
        "agent.invoke", "Bearer ", "Txn-Token", "access_token",
        "card_ingest_verified", "card_trust_ingest_verified", "innerHTML", "eval(",
    ):
        require(forbidden not in source, f"forbidden frontend primitive or payload field: {forbidden}")
    require(not re.search(r"https?://", styles), "stylesheet must not fetch external resources")
    require("const LIST_LIMIT = 200;" in source, "runtime list pages must use a 200-row ceiling")
    require(source.count("params: [ 'limit' ]") == 7, "every remaining list RPC must pass an explicit limit")
    require("callCrossDiscoveries" not in source and "s.tab('cross'" not in source,
            "experimental DNS discovery controls and polling must not be exposed")
    require(".prompt" not in source.lower() and "'prompt'" not in source.lower(),
            "UI must not bind a prompt field")
    require("tool arguments" in source.lower(), "overview must state its metadata-only boundary")
    require("Confirm peer trust" in source and "candidate generation" in source,
            "Peer Trust UI must require an explicit generation-bound confirmation")
    require("callPromote" in source and "callRevoke" in source and "callCardRevoke" in source,
            "Peer Trust UI must expose the approved confirm/revoke closure")
    require("Agent Router Quick Setup" in source and "Advanced Agent Router Settings" in source,
            "P7.5 must separate common setup from expert controls")
    require("routerIdentifier" in identity_source and "datatype = 'uciname'" not in identity_source,
            "Router and Peer IDs must use the native agentd identifier contract, not UCI section-name syntax")
    require("^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$" in source,
            "LuCI identifier validation must accept agentd dot, underscore and hyphen IDs")
    require("form.DynamicList" in source and "commaList" in source,
            "comma-separated operator inputs must use structured list editors")
    setup_source = (resources / "view" / "agent-router" / "setup.js").read_text(encoding="utf-8")
    settings_source = (resources / "view" / "agent-router" / "settings.js").read_text(encoding="utf-8")
    home_source = (resources / "view" / "agent-router" / "home.js").read_text(encoding="utf-8")
    mode_source = (resources / "agent-router" / "mode.js").read_text(encoding="utf-8")
    for mesh_view in (setup_source, settings_source):
        require("meshSetup.render({ clientOnly: true })" in mesh_view and
                "open_mesh_directory_connect_ipv4s" not in mesh_view and
                "open_mesh_directory_endpoints" not in mesh_view,
                "Quick and Advanced Setup must share guided Mesh controls, not manual URLs")
    require("admin/network/agent-router/diagnostics" not in menu and
            not (views / "agent-router" / "diagnostics.js").exists(),
            "Route Explain & Diagnostics must not be exposed in the user UI")
    require("Could not read capability routes" in source and "status is unavailable" in source,
            "RPC failures must not masquerade as empty routing state")
    require(menu["admin/network/agent-router/home"]["order"] == 5 and
            menu["admin/network/agent-router/home"]["action"]["path"] == "agent-router/home",
            "user mode must be the default Agent Routing child")
    require("admin/network/agent-router/developer" in menu and
            menu["admin/network/agent-router/developer"]["action"]["type"] == "firstchild",
            "developer views must remain under one explicit mode")
    legacy_aliases = [
        entry for path, entry in menu.items()
        if path.startswith("admin/network/agent-router/") and
        path.count("/") == 3 and entry.get("action", {}).get("type") == "alias"
    ]
    require(len(legacy_aliases) == 12 and all(entry.get("hidden") is True and
            entry.get("action", {}).get("path") == "admin/network/agent-router/home"
            for entry in legacy_aliases),
            "all twelve legacy developer URLs must return to User mode")
    require("User mode" in mode_source and "Developer mode" in mode_source and
            "localStorage" not in mode_source and "mode.preferred" not in home_source and
            "sessionStorage" in mode_source and "window.location.replace(target('user'))" in mode_source and
            "mode.enterUser();" in home_source and "mode.select('developer'" in home_source,
            "Developer mode must require an explicit, session-scoped action from User mode")
    developer_sources = [
        (resources / "view" / "agent-router" / f"{name}.js").read_text(encoding="utf-8")
        for name in ("overview", "setup", "cloud", "roles", "agents", "protocols",
                     "routes", "neighbors", "trust", "settings", "peers", "policies")
    ]
    require(all("mode.render('developer')" in item for item in developer_sources),
            "every developer page must provide a direct User mode switch")
    pairing_source = (resources / "agent-router" / "cloud-pairing.js").read_text(encoding="utf-8")
    require("object: 'nexus-agent-ui'" in home_source and
            "method: 'overview'" in home_source and
            "method: 'set_feature'" in home_source and
            "method: 'pair_cloud'" in pairing_source and "pairing.open" in home_source,
            "user mode must use the bounded RPC facade")
    for forbidden in ("AFIB", "ARPX", "Policy RIB", "route_id", "endpoint", "tenant", "public_ingress_port"):
        require(forbidden not in home_source, f"user mode exposes a developer-only field: {forbidden}")
    require("Cloud connection" in home_source and "Router network" in home_source and
            "Agent services" in home_source and "Agent capabilities" in home_source,
            "user mode must expose the three product controls and friendly route catalog")

    makefile = (package / "Makefile").read_text(encoding="utf-8")
    require("+luci-base +agentd" in makefile, "LuCI package must depend on the native agentd daemon")
    require("LUCI_PKGARCH:=all" in makefile, "LuCI package must be architecture independent")
    require("PKG_VERSION:=3.1.0" in makefile, "focused Router UI package version must be release locked")
    release = re.search(r"^PKG_RELEASE:=(\d+)$", makefile, re.M)
    require(release is not None and int(release.group(1)) >= 37,
            "LuCI release must include the synchronous-resolver health probe fix")
    require("+agent-netd" in makefile,
            "public IPv6 UI must install the restricted network executor")
    require("+agent-gw +agent-adapter" in makefile,
            "protocol UI must install the native gateway and adapter components")
    require("+nexus-cloud-connector" in makefile,
            "Cloud UI must install the renewable Edge registration connector")
    rpcd_path = package / "root" / "usr" / "libexec" / "rpcd" / "nexus-agent-ui"
    rpcd_source = rpcd_path.read_text(encoding="utf-8")
    require(rpcd_path.is_file() and "set_feature" in rpcd_source and "pair_cloud" in rpcd_source,
            "user-mode rpcd facade is missing")
    require("logger" not in rpcd_source and "sed '/pairing_code/d'" in rpcd_source,
            "pairing codes must not enter logs or configuration generations")
    require("router_mesh_mode" not in rpcd_source and "lan_auto_promotion_mode" not in rpcd_source and
            "peer_listen_port" not in rpcd_source and 'uci set "agent_gateway.main.lan_sdk_listen=' not in rpcd_source,
            "product switches must preserve developer policy, address and port settings")
    require("CONFIGURATION_CHANGED" in rpcd_source and "APPLY_FAILED" in rpcd_source and
            "restore_option" in rpcd_source,
            "user-mode mutations must use generation checks and rollback")
    for assignment in (
        "nexus_cloud.main.enabled=$enabled",
        "agent.main.peer_transport_enabled=$enabled",
        "agent.main.peer_listener_enabled=$enabled",
        "agent.main.discovery_enabled=$enabled",
        "agent.main.discovery_publish_enabled=$enabled",
        "agent_gateway.main.registration_enabled=$enabled",
        "agent_gateway.main.invoke_enabled=$enabled",
        "agent_gateway.main.lan_sdk_enabled=$enabled",
        "agent_gateway.main.lan_backend_enabled=$enabled",
        "agent_adapter.main.enabled=$enabled",
    ):
        require(assignment in rpcd_source, f"bounded feature mapping is missing: {assignment}")
    require("--connect-timeout 1 " not in rpcd_source,
            "OpenWrt synchronous curl must retain at least one full second for resolution")
    require(rpcd_source.count("--connect-timeout 2 --max-time 3 --max-filesize 65536") == 2,
            "Gateway and LAN metadata health checks must remain time/size bounded")
    zh_catalog = package / "po" / "zh_Hans" / "agent-router.po"
    require(zh_catalog.is_file() and 'msgid "User mode"' in zh_catalog.read_text(encoding="utf-8"),
            "user-mode Simplified Chinese catalog is missing")
    require("Public IPv6 ingress readiness" in settings_source and
            "public_ingress_port" in settings_source and
            "Agent call authentication" in settings_source and
            "Plain HTTP (no TLS)" in settings_source and
            "form.DummyValue, '_authentication_policy'" in settings_source and
            "public_transport" in settings_source,
            "P8.12.2 must expose the complete public /128 ingress prerequisites")
    require("Publish a direct connection descriptor" in settings_source and
            "public_tls_server_name" in settings_source and
            "public_ca_bundle_id" in settings_source,
            "P8.14 must expose friendly TLS identity and CA descriptor settings")
    require("MCP and A2A public ingress" in settings_source and
            "Allow MCP and A2A streaming" in settings_source,
            "P8.12.2 must expose protocol-adapter readiness without requiring raw UCI edits")
    require("object: 'network.interface'" in settings_source and
            "method: 'dump'" in settings_source and
            "Detected routed/PD prefix" in settings_source and
            "Detected upstream on-link /64" in settings_source and
            "detectPublicIpv6" in settings_source and
            "detectUpstreamOnlinkIpv6" in settings_source and
            "uci.set('agent', sectionId, 'public_ipv6_prefix', detected.prefix)" in settings_source,
            "Public Agent IPv6 must auto-detect and persist a routed WAN prefix")
    configure_read = acl["luci-app-agent-router-configure"]["read"]["ubus"]
    require(configure_read.get("network.interface") == ["dump"],
            "IPv6 auto-fill must have narrowly scoped network.interface dump access")
    overview_source = (resources / "view" / "agent-router" / "overview.js").read_text(encoding="utf-8")
    require("implemented_phase" not in overview_source and
            "Relay details" not in overview_source and
            "method: 'relay'" not in overview_source,
            "Overview must hide internal phase labels and Relay metadata details")
    agents_source = (resources / "view" / "agent-router" / "agents.js").read_text(encoding="utf-8")
    require("method: 'agents'" in agents_source, "Local Agents view must use the native agent.agents API")
    require("transport_connection_tracked" in agents_source,
            "Local Agents view must explain lease versus socket semantics")
    require("Copy IPv6 connection" in agents_source and
            "public_descriptor_enabled" in agents_source and
            "method: 'addresses'" in agents_source and
            "ipv6_prefix: routedPrefix" in agents_source,
            "P8.14 Local Agents must generate copyable per-/128 descriptors")
    roles_source = (resources / "view" / "agent-router" / "roles.js").read_text(encoding="utf-8")
    require("const roleMode = savedMode();" in roles_source and
            "roleSummary(roleMode, state)" in roles_source and
            "mode.render('developer')" in roles_source and
            "const mode = savedMode();" not in roles_source,
            "Router Roles must not shadow the shared mode renderer")
    require("meshSetup.render()" in roles_source,
            "Router Roles must use the product Mesh create/join workflow")
    require("nexus-agent-relayd" in roles_source and "nexus-agent-directoryd" in roles_source,
            "Router Roles must name the optional server packages")
    require("object: 'service'" in roles_source and "method: 'list'" in roles_source,
            "Router Roles must expose procd runtime status")
    require("Node only (recommended for normal LAN routers)" in roles_source,
            "Router Roles must provide a normal-router default instead of raw role switches")
    require("Role status" in roles_source and
            "ar-role-card" in roles_source and
            "Advanced role configuration" in roles_source,
            "Router Roles must use compact status cards and fold expert settings")
    require("fs.stat" in roles_source and "Component missing" in roles_source,
            "Router Roles must distinguish an uninstalled service from a stopped service")
    require("form.Flag, 'enabled'" not in roles_source,
            "Router Roles must not expose independent low-context service switches")
    require("public_hostname" not in roles_source and "public_port" not in roles_source,
            "Router Roles must not offer display-only identity settings")
    cloud_source = (resources / "view" / "agent-router" / "cloud.js").read_text(encoding="utf-8")
    require("pairing.open" in cloud_source and
            "Nexus Cloud connection" in cloud_source and
            "Device identity" in cloud_source and
            "Cloud lease duration" in cloud_source,
            "Cloud UI must guide enrollment, mTLS identity and renewable leases")
    require("Managed by Nexus Cloud" in cloud_source and
            "Check Device TLS" in cloud_source and
            "callTlsCheck" in cloud_source,
            "Cloud UI must default to managed identity and expose one-click TLS verification")
    require("Advanced / manual identity" not in cloud_source and
            "form.ListValue, 'identity_mode'" not in cloud_source and
            "form.DummyValue, '_identity_management'" in cloud_source,
            "Cloud identity must be a read-only summary, not a manual configuration form")
    require("replaces this Router’s existing Cloud enrollment" in pairing_source and
            "Cloud Relay unavailable" in cloud_source and
            "relayAvailable === false" in cloud_source and
            "value === 'relay' && relayAvailable === false" not in cloud_source and
            "poll.add" in cloud_source and
            "cloud_relay_available" in cloud_source and
            "relay_enabled" in cloud_source and
            "relay_ready" in cloud_source,
            "Cloud UI must explain stale Relay availability without blocking a fresh authenticated check")
    require("device_token" not in cloud_source and "Authorization" not in cloud_source,
            "Cloud UI must never read or display the device credential")
    require("form.DummyValue, '_cloud_origin'" in cloud_source and
            "form.Value, 'base_url'" not in cloud_source and
            "form.Value, 'enrollment_url'" not in cloud_source and
            "form.Value, 'pairing_code'" not in cloud_source,
            "both modes must use pairing links, never editable Cloud addresses or bare codes")
    require("localStorage" not in pairing_source and "sessionStorage" not in pairing_source and
            "uci.set" not in pairing_source and "window.location.reload()" in cloud_source,
            "pairing links must stay in memory and developer forms must reload managed values")
    protocols_source = (resources / "view" / "agent-router" / "protocols.js").read_text(encoding="utf-8")
    require("Agent APIs & Protocols" in protocols_source and
            "Allow Python SDK registration" in protocols_source and
            "Allow Agents to call other Agents" in protocols_source,
            "P8.2 must expose user-facing registration and invocation controls")
    require("MCP/A2A capability mappings" in protocols_source and
            "form.GridSection, 'mapping'" in protocols_source and
            "anonymous = true" in protocols_source,
            "P8.2 mappings must use a friendly anonymous row editor")
    require("agent_gateway" in protocols_source and "agent_adapter" in protocols_source and
            "synchronizedStreamFlag" in protocols_source,
            "P8.2 must synchronize the adapter and gateway data-plane switches")
    require("Python Agent Servers" in protocols_source and
            "Automatically call registered LAN Agents" in protocols_source and
            "lan_backend_enabled" in protocols_source and
            "Automatically call registered HTTPS Agents" in protocols_source and
            "HTTPS Agent certificate trust" in protocols_source and
            "remote_backend_ca_bundle_id" in protocols_source and
            "Fixed HTTPS endpoint mappings (legacy)" in protocols_source and
            "remote_backend_map" in protocols_source,
            "SDK-managed LAN and HTTPS Agents must be automatic while legacy TLS pins stay available")
    require("A2A setup assistant" in protocols_source and
            "Show Python example" in protocols_source and
            "NexusA2AClient" in protocols_source and
            "NexusA2AAgent" in protocols_source,
            "friendly A2A UI must guide mapping and generate caller/callee code")
    require("A2A caller URL" in protocols_source and
            "/message:stream for stream()" in protocols_source,
            "friendly A2A UI must expose the derived protocol endpoint")
    require("Authentication & trust" in protocols_source and
            "Managed by Nexus Cloud" in protocols_source and
            "cloudManagedAuthentication" in protocols_source and
            "Cloud authentication needs synchronization" in protocols_source,
            "authentication must be a truthful managed read-only summary")
    require("form.ListValue, '_jwt_auth_mode'" not in protocols_source and
            "form.Value, '_jwt_issuer'" not in protocols_source and
            "form.ListValue, 'auth_mode'" not in settings_source,
            "ordinary LuCI saves must not replace managed authentication")
    require("Automatic LAN authentication" in protocols_source and
            "lan_sdk_enabled" in protocols_source and
            "lan_sdk_listen" in protocols_source and
            "requireLanAccess" in protocols_source,
            "LAN examples must use automatic bootstrap and require an enabled listener")
    require("input_schema_json" in protocols_source and
            "Tool input schema (JSON)" in protocols_source and
            "Tool title" in protocols_source,
            "MCP mappings must expose optional discovery metadata")
    require("callService('agent-gw')" in protocols_source and
            "callService('agent-jwks')" in protocols_source and
            "callOverview()" in protocols_source and
            "Status unavailable" in protocols_source,
            "authentication summary must report actual configuration and unavailable RPC")
    require("form.DummyValue, '_python_agent_ca_policy'" in protocols_source and
            "separate from Cloud identity" in protocols_source and
            "form.Value, '_remote_backend_ca_file'" not in protocols_source,
            "independent backend trust must be preserved, not mistaken for Cloud trust")
    require("NEXUS_AGENT_TOKEN" in protocols_source and
            "NEXUS_AGENT_TRANSACTION_TOKEN" in protocols_source and
            "LuCI never stores or displays the token" in protocols_source,
            "P8.7 examples must inject short-lived credentials outside UCI")
    require("AutoTokenProvider" in protocols_source,
            "P8.20 FastMCP example must support router-discovered OIDC credentials")
    require("client.stream" in protocols_source and
            "message:send / message:stream" in protocols_source,
            "P8.7 UI example must use the high-level A2A streaming API")
    require("Show streaming FastMCP example" in protocols_source and
            "FastMCPBridge" in protocols_source and
            "ctx.report_progress" in protocols_source,
            "P8.8 UI must generate a real FastMCP progress bridge")
    require("MCP caller URL" in protocols_source and
            "application/json, text/event-stream" in protocols_source and
            "NEXUS_AGENT_TOKEN" in protocols_source,
            "P8.8 UI must expose the MCP stream endpoint and external JWT injection")
    require("Resume long-running calls after a disconnect" in protocols_source and
            "stream_resume_capacity" in protocols_source and
            "stream_resume_ttl_seconds" in protocols_source and
            "Last-Event-ID" in protocols_source and
            "event.event_id" in protocols_source,
            "P8.9 UI must expose bounded resume policy and generated client guidance")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

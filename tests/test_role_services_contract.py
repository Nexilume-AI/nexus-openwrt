#!/usr/bin/env python3
"""Static P7.6 contract checks for optional OpenWrt server roles."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    package = root / "feed" / "nexus-agent-services"
    makefile = (package / "Makefile").read_text(encoding="utf-8")
    roles = (package / "files" / "nexus_roles.config").read_text(encoding="utf-8")
    agentctl = (root / "feed" / "agentd" / "files" / "agentctl").read_text(encoding="utf-8")
    relay_init = (package / "files" / "nexus-relayd.init").read_text(encoding="utf-8")
    directory_init = (package / "files" / "nexus-directoryd.init").read_text(encoding="utf-8")
    seed_setup = (package / "files" / "nexus-open-mesh-seed-setup").read_text(
        encoding="utf-8"
    )

    for name in ("nexus-agent-roles", "nexus-agent-relayd", "nexus-agent-directoryd"):
        require(f"Package/{name}" in makefile, f"missing OpenWrt package {name}")
        require(f"BuildPackage,{name}" in makefile, f"package {name} is not emitted")
    require(makefile.count("+nexus-node-runtime") == 2,
            "only Relay and Directory server packages must require the target Node runtime")
    require("+nexus-agent-roles +nexus-node-runtime" in makefile,
            "server roles must share the UCI role package and target runtime")
    require("relay/*.js" in makefile and "directory/nexus-directory.js" in makefile,
            "server packages must install the accepted P6 runtime")

    require("config role 'relay'" in roles and "config role 'directory'" in roles,
            "shared UCI config must expose both server roles")
    require("config profile 'main'" in roles and "option mode 'node'" in roles,
            "shared UCI config must default to the guided Node-only profile")
    require(roles.count("option enabled '0'") == 2, "server roles must be disabled by default")
    require("agentctl agents [limit]" in agentctl and "routes|agents|neighbors" in agentctl,
            "native CLI must expose the bounded local-Agent API")

    for name, init, user, binary in (
        ("Relay", relay_init, "nexus-relay", "/usr/lib/nexus-relayd/nexus-relayd.js"),
        ("Directory", directory_init, "nexus-directory", "/usr/lib/nexus-directory/nexus-directory.js"),
    ):
        require("USE_PROCD=1" in init, f"{name} role must be supervised by procd")
        require("config_get_bool enabled" in init and "[ \"$enabled\" = 1 ]" in init,
                f"{name} role must honor disabled-by-default UCI")
        require("config_get mode main mode" in init and "|all) enabled=1" in init,
                f"{name} role must honor the guided deployment profile")
        require("procd_add_reload_trigger nexus_roles" in init,
                f"{name} role must reload after LuCI apply")
        require(f"procd_set_param user {user}" in init, f"{name} role must use its service identity")
        require(binary in init, f"{name} role command path differs from package install path")
        require("procd_set_param respawn" in init and "procd_set_param limits" in init,
                f"{name} role lacks bounded restart/resource supervision")

    migration = (package / "files" / "99-nexus-agent-roles-profile").read_text(encoding="utf-8")
    require("relay.enabled" in migration and "directory.enabled" in migration and "mode='all'" in migration,
            "role profile migration must preserve legacy Relay and Directory choices")
    require("99-nexus-agent-roles-profile" in makefile,
            "role profile migration must be installed as a UCI default")
    require("nexus-open-mesh-seed-setup" in makefile,
            "the zero-configuration Open Mesh seed setup command must be packaged")
    require("/v1/open-mesh/assignment" in seed_setup and "default-open-mesh" in seed_setup,
            "seed setup must expose the dedicated default Mesh realm endpoint")
    require("cloudIngress\": {\"enabled\": false}" in seed_setup,
            "the Open Mesh seed must not implicitly enable Cloud ingress")
    require("nexus_cloud" not in seed_setup and "agent_router" not in seed_setup,
            "seed setup must not rewrite Cloud enrollment or Agent Router state")
    seed_builder = (root / "scripts" / "build-open-mesh-seed-sdk.sh").read_text(
        encoding="utf-8"
    )
    require("package/feeds/nexus_agent_router/nexus-node-runtime/compile" in seed_builder,
            "the seed build must produce the target Node runtime")
    require("nexus-agent-relayd" in seed_builder and
            "nexus-agent-directoryd" in seed_builder,
            "the seed build must produce both OpenWrt server roles")

    for relative in ("relay/relay.config.example.json", "directory/directory.config.example.json"):
        config = json.loads((root / relative).read_text(encoding="utf-8"))
        require(set(config["ticketKeys"].values()) == {"GENERATE_SHARED_TICKET_KEY"},
                "packaged service defaults must not contain a usable signing key")
    require("openssl rand -base64 48" in seed_setup and
            seed_setup.count('"$ticket_key_id": "$ticket_secret"') == 2,
            "seed setup must install the same newly generated key in both services")
    print("P7.6 role service contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Static contract checks for local DNSSEC resolver auto-detection."""

import sys
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    init = (root / "feed/agentd/files/agentd.init").read_text(encoding="utf-8")
    config = (root / "feed/agentd/files/agentd.config").read_text(encoding="utf-8")
    agentd = (root / "feed/agentd/src/agentd.c").read_text(encoding="utf-8")
    settings = (
        root
        / "feed/luci-app-agent-router/htdocs/luci-static/resources/view/agent-router/settings.js"
    ).read_text(encoding="utf-8")

    require("cross_discovery_resolver_mode 'auto'" in config,
            "resolver auto-detection must be the packaged default")
    for marker in (
        "detect_cross_resolver_port", "/proc/net/udp",
        "unbound.@unbound[0].listen_port", "dhcp.@dnsmasq[0].dnssec",
        "pidof unbound", "cross_resolver_ipv4=127.0.0.1",
        "NEXUS_CROSS_RESOLVER_DETECTED", "invalid cross-discovery resolver mode",
    ):
        require(marker in init, f"native resolver detection contract missing: {marker}")

    require('"cross_discovery_resolver_ipv4"' in agentd and
            '"cross_discovery_resolver_port"' in agentd and
            '"cross_discovery_resolver_detected"' in agentd,
            "agent.stats must expose the effective resolver endpoint")
    # DNS federation remains native backwards compatibility, not a new-user
    # configuration surface. Open Mesh seed joining is the supported UI path.
    require("Local DNSSEC resolver" not in settings and
            "depends('cross_discovery_resolver_mode', 'manual')" not in settings,
            "LuCI must not reintroduce removed DNS federation editors")
    require("Open Mesh" in settings and "meshSetup.render({ clientOnly: true })" in settings,
            "LuCI must retain the separate link-based Open Mesh client controls")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

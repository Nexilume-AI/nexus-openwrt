#!/usr/bin/env python3
"""Static cross-component contract for P8.11 Directory DNS-first bootstrap."""

from __future__ import annotations

import sys
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    directory = (root / "feed/agentd/src/agent_relay_directory.c").read_text(
        encoding="utf-8"
    )
    bootstrap = (root / "feed/agentd/src/agent_relay_bootstrap.c").read_text(
        encoding="utf-8"
    )
    agentd = (root / "feed/agentd/src/agentd.c").read_text(encoding="utf-8")
    init = (root / "feed/agentd/files/agentd.init").read_text(encoding="utf-8")
    config = (root / "feed/agentd/files/agentd.config").read_text(encoding="utf-8")
    overview = (
        root
        / "feed/luci-app-agent-router/htdocs/luci-static/resources/view/agent-router/overview.js"
    ).read_text(encoding="utf-8")

    require(
        "connect_ipv4[0] != '\\0'" in directory
        and "dns_mode = connect_ipv4s[0] == '\\0'" in directory,
        "Directory endpoint parser must accept only an entirely empty optional pin set",
    )
    require(
        "getaddrinfo(endpoint->server_identity" in bootstrap
        and "hints.ai_family = AF_INET" in bootstrap
        and "freeaddrinfo(addresses)" in bootstrap,
        "bootstrap must resolve unpinned endpoint hostnames to bounded IPv4 output",
    )
    require(
        "mbedtls_ssl_set_hostname" in bootstrap
        and "endpoint->server_identity" in bootstrap,
        "DNS/pin selection must not replace TLS hostname verification",
    )
    require(
        'AGENTD_DEFAULT_RELAY_DIRECTORY_IPV4 ""' in agentd
        and "option relay_directory_connect_ipv4 ''" in config,
        "new installations and direct daemon startup must default to DNS",
    )
    require(
        'if [ -n "$relay_directory_endpoints" ]; then' in init
        and 'relay_directory_connect_ipv4s="$relay_directory_connect_ipv4"' in init
        and '[ -z "$relay_directory_connect_ipv4s" ] ||' in init,
        "init compatibility must retain singular pins only for singular endpoints",
    )
    require(
        "directory_dns" in agentd
        and "directory_server_identity" in agentd
        and "directory_resolved_ipv4" in agentd
        and "Directory address source" not in overview,
        "ubus must retain the Directory decision while Overview hides low-level Relay metadata",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

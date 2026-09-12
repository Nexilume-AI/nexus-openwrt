#!/usr/bin/env python3
"""Static guard for seamless Relay ticket renewal in the OpenWrt client."""

from pathlib import Path
import sys


def require(text: str, value: str, label: str) -> None:
    if value not in text:
        raise AssertionError(f"missing {label}: {value}")


def main() -> None:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    source = (root / "feed/agentd/src/agent_peer_transport.c").read_text(encoding="utf-8")
    header = (root / "feed/agentd/src/agent_peer_transport.h").read_text(encoding="utf-8")
    relay = (root / "relay/nexus-relayd.js").read_text(encoding="utf-8")

    for value, label in (
        ('#define AGENT_RELAY_TICKET_REFRESH_PATH "/relay/ticket/refresh/v1"', "refresh path"),
        ("static bool submit_ticket_refresh", "bounded refresh submitter"),
        ("slot->phase == TRANSPORT_ESTABLISHED", "established-session gate"),
        ("MAKE_SENSITIVE_NV(\"nexus-relay-ticket\"", "never-index ticket header"),
        ('value_equals(\n                value, value_length, "204")', "204 response contract"),
        ("Relay ticket refresh response rejected", "fail-closed response"),
        ("strcmp(slot->relay_session_ticket, session_ticket) == 0", "idempotent update"),
        ("!submit_ticket_refresh(slot)", "refresh submission enforcement"),
    ):
        require(source, value, label)
    require(header, "Refreshes an established Relay session in-band", "public behavior contract")
    for value, label in (
        ('const TICKET_REFRESH_PATH = "/relay/ticket/refresh/v1"', "Relay refresh path"),
        ("ticketRefresh && state.active", "active-session refresh gate"),
        ("Relay ticket refresh identity mismatch", "identity binding"),
        ('capture("ticket-refreshed"', "non-secret audit event"),
        ('stream.respond({ ":status": 204 })', "accepted refresh status"),
        ("armTicketExpiry(state, claims)", "expiry rollover"),
    ):
        require(relay, value, label)
    print("Agent Relay ticket refresh contract passed")


if __name__ == "__main__":
    main()

---
sidebar_position: 4
title: Router Roles
---

# Router Roles

Every Router is a Node. Relay and Directory are optional server roles.

| Field | Values/purpose | When to use |
| --- | --- | --- |
| Hosted services | Node only, Node + Relay, Node + Directory, or all | Normal Routers use Node only |
| Relay configuration file | Relay JSON, default `/etc/nexus-relayd/relay.json` | Change only when hosting Relay |
| Directory public hostname | Name used by other Routers and covered by certificate SAN | Only when hosting Directory |
| Directory public port | Externally reachable port, default `8443` | Match firewall/NAT mapping |
| Directory configuration file | Default `/etc/nexus-directoryd/directory.json` | Change only for custom JSON |

Readiness cards show selection, package installation, configuration presence, and runtime state. Selecting a role does not install missing packages or create production TLS. If a saved role remains **Ready to start**, inspect the system log.

These roles host Open Mesh services on this Router. Use [Nexus Cloud](../../guides/cloud-relay.md) for Cloud Relay rather than switching server roles.

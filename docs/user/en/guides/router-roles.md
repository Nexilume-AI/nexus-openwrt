---
sidebar_position: 1
title: Configure private-cloud node roles
---

# Configure Agent private-cloud node roles

Every Nexus Router is a Node in the Agent private cloud. Relay and Directory are optional server roles, not Edge, Hub, or Leaf templates.

| LuCI mode | Purpose | Extra package |
| --- | --- | --- |
| Node only | Normal private-cloud node that registers, publishes, and invokes Agents | None |
| Node + Relay | Carries cross-NAT Agent traffic for other nodes | `nexus-agent-relayd` |
| Node + Directory | Assigns trusted Relays to nodes | `nexus-agent-directoryd` |
| Node + Relay + Directory | Small self-hosted control node with both roles | Both |

## Procedure

1. Open **Status → Agent Routing → Developer mode → Router Roles**.
2. Choose a mode under **Hosted services**.
3. Read **Role status**. Component, Configuration, and Runtime should match the selected role.
4. Change a path under **Advanced role configuration** only when maintaining custom JSON.
5. For Directory, enter a **Public hostname** covered by its certificate and an externally reachable port.
6. Select **Save & Apply** and confirm that the selected role reports Ready.

## Default files and services

| Role | Service | Default configuration |
| --- | --- | --- |
| Relay | `/etc/init.d/nexus-relayd` | `/etc/nexus-relayd/relay.json` |
| Directory | `/etc/init.d/nexus-directoryd` | `/etc/nexus-directoryd/directory.json` |

The Directory page produces an assignment URL for other nodes: `https://HOST:PORT/v1/open-mesh/assignment`.

## Verify

```sh
/etc/init.d/nexus-relayd running
/etc/init.d/nexus-directoryd running
logread | grep -E 'nexus-relayd|nexus-directoryd' | tail -n 80
```

Check only selected roles. If a component is missing, install the package named by LuCI. If configuration is missing or contains placeholders, configure valid certificates, endpoints and service identities; copying example JSON alone does not make the service ready.

## Selection guidance

Most private-cloud nodes use Node only. A Relay needs a network entry point reachable by participating nodes. A Directory needs a stable hostname, TLS certificate, and an operations owner. Choose all roles only when the team intentionally operates both services.

The Relay bundled with Cloud Community runs on the Cloud server independently of these self-hosted roles; see [Cloud Relay](cloud-relay.md). Clients copy the full Directory-provided endpoint; a Cloud tunnel address is not an Open Mesh assignment URL.

## Supplied seed initialization script

The `nexus-agent-roles` package provides `/usr/sbin/nexus-open-mesh-seed-setup` for an intentionally hosted seed. It requires Node, OpenSSL, Relay/Directory service accounts and a WAN firewall zone. It writes certificates and service configuration and changes firewall settings; it is not a read-only check for ordinary nodes. Local Relay/Directory ports default to `17444` / `18443`, distinct from Cloud Community ports `27444` / `27445`. Back up existing custom configuration and inspect the script before running it. Ordinary clients only need the seed-provided assignment URL.

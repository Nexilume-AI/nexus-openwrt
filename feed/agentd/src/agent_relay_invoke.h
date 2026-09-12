#ifndef NEXUS_AGENT_RELAY_INVOKE_H
#define NEXUS_AGENT_RELAY_INVOKE_H

#include "agent_ipc_server.h"
#include "agent_peer_transport.h"
#include "agent_forwarding_assertion.h"
#include "route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct agent_relay_invoke_stats {
    size_t active;
    uint64_t source_started;
    uint64_t target_started;
    uint64_t transit_started;
    uint64_t transit_completed;
    uint64_t transit_failed;
    uint64_t transit_no_route;
    uint64_t transit_hop_limit_rejected;
    uint64_t transit_bytes_forwarded;
    uint64_t completed;
    uint64_t failed;
    uint64_t timed_out;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t streams_started;
    uint64_t streams_completed;
    uint64_t stream_events;
    uint64_t stream_bytes;
    uint64_t stream_backpressure_resets;
    uint64_t forwarding_assertions_verified;
    uint64_t forwarding_assertions_rejected;
    uint64_t forwarding_assertion_replays;
    uint64_t forwarding_assertion_body_mismatches;
    uint64_t cloud_tenant_aliases;
    uint64_t cloud_tenant_alias_rejections;
    uint64_t internal_gateway_started;
    uint64_t internal_gateway_completed;
    uint64_t internal_gateway_failed;
    uint64_t internal_gateway_unavailable;
};

struct agent_relay_invoke_auth_config {
    bool required;
    const char *public_key_file;
    const char *key_id;
    const char *issuer;
    const char *source_router_id;
    uint32_t clock_skew_seconds;
    uint32_t max_ttl_seconds;
    size_t replay_capacity;
};

struct agent_relay_invoke_gateway_config {
    bool enabled;
    const char *endpoint;
    const char *token_file;
};

typedef bool (*agent_relay_tenant_alias_resolver)(
    const char *cloud_tenant,
    const char *target_agent,
    const char *intent,
    char local_tenant[AGENT_IPC_TENANT_LEN],
    void *context
);

struct agent_relay_invoke_alias_config {
    agent_relay_tenant_alias_resolver resolve;
    void *context;
};

struct agent_relay_invoke_manager;
struct agent_peer_listener;

struct agent_relay_invoke_manager *agent_relay_invoke_create(
    struct route_table *routes,
    struct agent_peer_transport_manager *transport,
    const char *local_router_id,
    uint64_t (*now_ms)(void),
    const struct agent_relay_invoke_auth_config *auth,
    const struct agent_relay_invoke_gateway_config *gateway,
    const struct agent_relay_invoke_alias_config *alias
);

void agent_relay_invoke_destroy(struct agent_relay_invoke_manager *manager);

void agent_relay_invoke_set_listener(
    struct agent_relay_invoke_manager *manager,
    struct agent_peer_listener *listener
);

void agent_relay_invoke_set_open_mesh(
    struct agent_relay_invoke_manager *manager,
    struct peer_table *peers,
    bool enabled
);

bool agent_relay_invoke_from_ipc(
    struct agent_relay_invoke_manager *manager,
    struct agent_ipc_client *client,
    const struct agent_ipc_invoke_request *request
);

void agent_relay_invoke_cancel_ipc(
    struct agent_relay_invoke_manager *manager,
    struct agent_ipc_client *client
);

bool agent_relay_invoke_on_tunnel(
    struct agent_relay_invoke_manager *manager,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message,
    uint64_t now_ms
);

void agent_relay_invoke_get_stats(
    const struct agent_relay_invoke_manager *manager,
    struct agent_relay_invoke_stats *stats
);

#endif

#ifndef NEXUS_AGENT_PEER_ROUTES_H
#define NEXUS_AGENT_PEER_ROUTES_H

#include "agent_arpx_protocol.h"
#include "peer_table.h"
#include "route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum agent_peer_route_result {
    AGENT_PEER_ROUTE_OK = 0,
    AGENT_PEER_ROUTE_INVALID = -1,
    AGENT_PEER_ROUTE_LOOP = -2,
    AGENT_PEER_ROUTE_OWNERSHIP = -3,
    AGENT_PEER_ROUTE_TABLE_ERROR = -4,
    AGENT_PEER_ROUTE_UNAUTHORIZED = -5
};

typedef bool (*agent_peer_route_authorize_handler)(
    void *context,
    const struct agent_peer *peer,
    const struct agent_arpx_message *message,
    uint64_t now_ms
);

typedef void (*agent_peer_route_removed_handler)(
    void *context,
    const struct agent_route *route
);

struct agent_peer_route_manager {
    char local_router_id[AGENT_ROUTER_ID_LEN];
    struct route_table *routes;
    struct peer_table *peers;
    bool open_mesh;
    agent_peer_route_authorize_handler authorize;
    void *authorize_context;
    uint64_t updates_accepted;
    uint64_t withdrawals_accepted;
    uint64_t loops_rejected;
    uint64_t ownership_rejected;
    uint64_t authorization_rejected;
    uint64_t routes_staled;
    uint64_t routes_removed;
    uint64_t snapshots_started;
    uint64_t snapshots_completed;
    uint64_t snapshot_routes_removed;
};

bool agent_peer_routes_init(
    struct agent_peer_route_manager *manager,
    const char *local_router_id,
    struct route_table *routes,
    struct peer_table *peers
);

void agent_peer_routes_set_authorizer(
    struct agent_peer_route_manager *manager,
    agent_peer_route_authorize_handler authorize,
    void *context
);

void agent_peer_routes_set_open_mesh(
    struct agent_peer_route_manager *manager,
    bool enabled
);

enum agent_peer_route_result agent_peer_routes_on_message(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    const struct agent_arpx_message *message,
    uint64_t now_ms
);

void agent_peer_routes_session_up(
    struct agent_peer_route_manager *manager,
    const char *peer_id
);

size_t agent_peer_routes_session_down(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    uint64_t now_ms
);

size_t agent_peer_routes_remove_peer_routes(
    struct agent_peer_route_manager *manager,
    const char *peer_id
);

size_t agent_peer_routes_prune(
    struct agent_peer_route_manager *manager,
    uint64_t now_ms
);

size_t agent_peer_routes_flush(
    struct agent_peer_route_manager *manager
);

bool agent_peer_routes_snapshot_begin(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    uint64_t snapshot_id
);

enum agent_peer_route_result agent_peer_routes_snapshot_end(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    uint64_t snapshot_id,
    agent_peer_route_removed_handler removed_handler,
    void *removed_context
);

bool agent_peer_routes_export_update(
    const struct agent_route *route,
    const char *local_router_id,
    const struct agent_peer *target_peer,
    bool reflector_enabled,
    uint64_t now_ms,
    struct agent_arpx_message *message
);

bool agent_peer_routes_make_update(
    const struct agent_route *route,
    const char *local_router_id,
    uint64_t now_ms,
    struct agent_arpx_message *message
);

bool agent_peer_routes_make_withdraw(
    const char *route_id,
    struct agent_arpx_message *message
);

bool agent_peer_routes_make_snapshot_request(
    uint64_t snapshot_id,
    struct agent_arpx_message *message
);

bool agent_peer_routes_make_snapshot_end(
    uint64_t snapshot_id,
    struct agent_arpx_message *message
);

#endif

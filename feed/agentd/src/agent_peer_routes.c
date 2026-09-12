#include "agent_peer_routes.h"

#include <stdio.h>
#include <string.h>

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || capacity == 0U || source == NULL) {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static struct agent_peer *mutable_peer(
    struct agent_peer_route_manager *manager,
    const char *peer_id
)
{
    struct agent_peer *peer;

    if (manager == NULL || manager->peers == NULL || peer_id == NULL) {
        return NULL;
    }
    for (peer = manager->peers->head; peer != NULL; peer = peer->next) {
        if (strcmp(peer->peer_id, peer_id) == 0) {
            return peer;
        }
    }
    return NULL;
}

static void recount_peer(
    struct agent_peer_route_manager *manager,
    struct agent_peer *peer
)
{
    const struct agent_route *route;
    size_t count = 0U;

    if (peer == NULL) {
        return;
    }
    for (route = route_table_first(manager->routes); route != NULL;
         route = route->next) {
        if (route->source == AGENT_ROUTE_SOURCE_PEER &&
            strcmp(route->learned_from_peer, peer->peer_id) == 0) {
            count++;
        }
    }
    peer->learned_routes = count;
}

bool agent_peer_routes_init(
    struct agent_peer_route_manager *manager,
    const char *local_router_id,
    struct route_table *routes,
    struct peer_table *peers
)
{
    struct agent_arpx_message probe;

    if (manager == NULL || routes == NULL || peers == NULL) {
        return false;
    }
    memset(&probe, 0, sizeof(probe));
    probe.version = AGENT_ARPX_VERSION;
    probe.type = AGENT_ARPX_OPEN;
    if (!copy_text(probe.router_id, sizeof(probe.router_id),
                   local_router_id) ||
        !copy_text(probe.domain_id, sizeof(probe.domain_id), "local.test")) {
        return false;
    }
    probe.boot_epoch = 1U;
    probe.sequence = 1U;
    probe.heartbeat_ms = AGENT_ARPX_MIN_HEARTBEAT_MS;
    if (agent_arpx_message_validate(&probe) != AGENT_ARPX_OK) {
        return false;
    }
    memset(manager, 0, sizeof(*manager));
    if (!copy_text(manager->local_router_id,
                   sizeof(manager->local_router_id), local_router_id)) {
        return false;
    }
    manager->routes = routes;
    manager->peers = peers;
    return true;
}

void agent_peer_routes_set_authorizer(
    struct agent_peer_route_manager *manager,
    agent_peer_route_authorize_handler authorize,
    void *context
)
{
    if (manager == NULL) {
        return;
    }
    manager->authorize = authorize;
    manager->authorize_context = context;
}

void agent_peer_routes_set_open_mesh(
    struct agent_peer_route_manager *manager,
    bool enabled
)
{
    if (manager != NULL) manager->open_mesh = enabled;
}

static bool path_contains(
    const struct agent_arpx_message *message,
    const char *router_id
)
{
    size_t i;

    for (i = 0U; i < message->path_length; i++) {
        if (strcmp(message->path[i], router_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool route_path_contains(
    const struct agent_route *route,
    const char *router_id
)
{
    size_t i;

    for (i = 0U; i < route->path_length; i++) {
        if (strcmp(route->path[i], router_id) == 0) {
            return true;
        }
    }
    return false;
}

static enum agent_peer_route_result accept_update(
    struct agent_peer_route_manager *manager,
    struct agent_peer *peer,
    const struct agent_arpx_message *message,
    uint64_t now_ms
)
{
    const struct agent_route *existing;
    struct agent_route route;
    size_t i;

    if (path_contains(message, manager->local_router_id)) {
        manager->loops_rejected++;
        return AGENT_PEER_ROUTE_LOOP;
    }
    if (manager->authorize != NULL &&
        !manager->authorize(manager->authorize_context, peer, message,
                            now_ms)) {
        manager->authorization_rejected++;
        return AGENT_PEER_ROUTE_UNAUTHORIZED;
    }
    existing = route_table_find(manager->routes, message->route_id);
    if (existing != NULL &&
        (existing->source != AGENT_ROUTE_SOURCE_PEER ||
         strcmp(existing->learned_from_peer, peer->peer_id) != 0)) {
        manager->ownership_rejected++;
        return AGENT_PEER_ROUTE_OWNERSHIP;
    }

    memset(&route, 0, sizeof(route));
    if (!copy_text(route.route_id, sizeof(route.route_id), message->route_id) ||
        !copy_text(route.intent, sizeof(route.intent), message->intent) ||
        !copy_text(route.origin, sizeof(route.origin), message->origin) ||
        !copy_text(route.endpoint, sizeof(route.endpoint), message->endpoint) ||
        !copy_text(route.tenant, sizeof(route.tenant),
                   manager->open_mesh &&
                       (peer->role != AGENT_PEER_ROLE_RELAY ||
                        peer->open_mesh)
                       ? "*" : message->tenant) ||
        !copy_text(route.region, sizeof(route.region), message->region) ||
        !copy_text(route.learned_from_peer,
                   sizeof(route.learned_from_peer), peer->peer_id)) {
        return AGENT_PEER_ROUTE_INVALID;
    }
    route.version = message->capability_version;
    route.cost_microunits = message->cost_microunits;
    route.latency_ms = message->latency_ms;
    route.trust_level = message->trust_level;
    route.load_permille = message->load_permille;
    route.hop_count = message->path_length;
    route.healthy = true;
    route.lease_expires_ms = saturating_add(
        now_ms, message->remaining_lease_ms);
    route.sequence = message->sequence;
    route.source = AGENT_ROUTE_SOURCE_PEER;
    route.snapshot_id = peer->snapshot_receiving ? peer->snapshot_id : 0U;
    route.path_length = message->path_length;
    for (i = 0U; i < message->path_length; i++) {
        if (!copy_text(route.path[i], sizeof(route.path[i]),
                       message->path[i])) {
            return AGENT_PEER_ROUTE_INVALID;
        }
    }
    if (route_table_upsert(manager->routes, &route) != ROUTE_TABLE_OK) {
        return AGENT_PEER_ROUTE_TABLE_ERROR;
    }
    manager->updates_accepted++;
    recount_peer(manager, peer);
    return AGENT_PEER_ROUTE_OK;
}

static enum agent_peer_route_result accept_withdraw(
    struct agent_peer_route_manager *manager,
    struct agent_peer *peer,
    const struct agent_arpx_message *message
)
{
    const struct agent_route *existing = route_table_find(
        manager->routes, message->route_id);

    if (existing == NULL) {
        manager->withdrawals_accepted++;
        return AGENT_PEER_ROUTE_OK;
    }
    if (existing->source != AGENT_ROUTE_SOURCE_PEER ||
        strcmp(existing->learned_from_peer, peer->peer_id) != 0) {
        manager->ownership_rejected++;
        return AGENT_PEER_ROUTE_OWNERSHIP;
    }
    if (route_table_remove(manager->routes, message->route_id) !=
        ROUTE_TABLE_OK) {
        return AGENT_PEER_ROUTE_TABLE_ERROR;
    }
    manager->withdrawals_accepted++;
    manager->routes_removed++;
    recount_peer(manager, peer);
    return AGENT_PEER_ROUTE_OK;
}

enum agent_peer_route_result agent_peer_routes_on_message(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    const struct agent_arpx_message *message,
    uint64_t now_ms
)
{
    struct agent_peer *peer = mutable_peer(manager, peer_id);

    if (manager == NULL || message == NULL || peer == NULL ||
        agent_arpx_message_validate(message) != AGENT_ARPX_OK ||
        strcmp(message->router_id, peer->router_id) != 0 ||
        strcmp(message->domain_id, peer->domain_id) != 0) {
        return AGENT_PEER_ROUTE_INVALID;
    }
    if (message->type == AGENT_ARPX_CAPABILITY_UPDATE) {
        return accept_update(manager, peer, message, now_ms);
    }
    if (message->type == AGENT_ARPX_CAPABILITY_WITHDRAW) {
        return accept_withdraw(manager, peer, message);
    }
    return AGENT_PEER_ROUTE_INVALID;
}

void agent_peer_routes_session_up(
    struct agent_peer_route_manager *manager,
    const char *peer_id
)
{
    struct agent_peer *peer = mutable_peer(manager, peer_id);

    if (peer != NULL) {
        peer->stale_until_ms = 0U;
        peer->snapshot_receiving = false;
        peer->snapshot_id = 0U;
        (void)peer_table_set_state(manager->peers, peer_id,
                                   AGENT_PEER_STATE_ESTABLISHED);
    }
}

size_t agent_peer_routes_session_down(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    uint64_t now_ms
)
{
    struct agent_peer *peer = mutable_peer(manager, peer_id);
    struct agent_route *route;
    uint64_t stale_until;
    size_t staled = 0U;

    if (peer == NULL) {
        return 0U;
    }
    peer->snapshot_receiving = false;
    peer->snapshot_id = 0U;
    stale_until = saturating_add(
        now_ms, (uint64_t)peer->graceful_restart_seconds * 1000U);
    for (route = manager->routes->head; route != NULL; route = route->next) {
        if (route->source == AGENT_ROUTE_SOURCE_PEER &&
            strcmp(route->learned_from_peer, peer_id) == 0) {
            route->healthy = false;
            if (route->lease_expires_ms > stale_until) {
                route->lease_expires_ms = stale_until;
            }
            staled++;
        }
    }
    if (staled > 0U) {
        manager->routes->generation++;
        manager->routes_staled += staled;
        peer->stale_until_ms = stale_until;
        (void)peer_table_set_state(manager->peers, peer_id,
                                   AGENT_PEER_STATE_STALE);
    } else {
        (void)peer_table_set_state(manager->peers, peer_id,
                                   AGENT_PEER_STATE_DOWN);
    }
    return staled;
}

size_t agent_peer_routes_remove_peer_routes(
    struct agent_peer_route_manager *manager,
    const char *peer_id
)
{
    struct agent_peer *peer;
    struct agent_route *route;
    struct agent_route *next;
    size_t removed = 0U;

    if (manager == NULL || manager->routes == NULL ||
        manager->peers == NULL || peer_id == NULL || peer_id[0] == '\0') {
        return 0U;
    }
    route = manager->routes->head;
    while (route != NULL) {
        next = route->next;
        if (route->source == AGENT_ROUTE_SOURCE_PEER &&
            strcmp(route->learned_from_peer, peer_id) == 0 &&
            route_table_remove(manager->routes, route->route_id) ==
                ROUTE_TABLE_OK) {
            removed++;
        }
        route = next;
    }
    manager->routes_removed += removed;
    peer = mutable_peer(manager, peer_id);
    if (peer != NULL) {
        peer->learned_routes = 0U;
        peer->stale_until_ms = 0U;
        peer->snapshot_receiving = false;
        peer->snapshot_id = 0U;
    }
    return removed;
}

size_t agent_peer_routes_prune(
    struct agent_peer_route_manager *manager,
    uint64_t now_ms
)
{
    struct agent_peer *peer;
    size_t before;
    size_t removed;

    if (manager == NULL || manager->routes == NULL) {
        return 0U;
    }
    before = manager->routes->count;
    (void)route_table_prune_expired(manager->routes, now_ms);
    removed = before - manager->routes->count;
    manager->routes_removed += removed;
    for (peer = manager->peers->head; peer != NULL; peer = peer->next) {
        recount_peer(manager, peer);
        if (peer->state == AGENT_PEER_STATE_STALE &&
            peer->stale_until_ms != 0U && now_ms >= peer->stale_until_ms) {
            peer->stale_until_ms = 0U;
            (void)peer_table_set_state(manager->peers, peer->peer_id,
                                       AGENT_PEER_STATE_DOWN);
        }
    }
    return removed;
}

size_t agent_peer_routes_flush(
    struct agent_peer_route_manager *manager
)
{
    struct agent_route *route;
    struct agent_route *next;
    struct agent_peer *peer;
    size_t removed = 0U;

    if (manager == NULL || manager->routes == NULL) {
        return 0U;
    }
    route = manager->routes->head;
    while (route != NULL) {
        next = route->next;
        if (route->source == AGENT_ROUTE_SOURCE_PEER &&
            route_table_remove(manager->routes, route->route_id) ==
                ROUTE_TABLE_OK) {
            removed++;
        }
        route = next;
    }
    manager->routes_removed += removed;
    for (peer = manager->peers->head; peer != NULL; peer = peer->next) {
        peer->learned_routes = 0U;
        peer->stale_until_ms = 0U;
        peer->snapshot_receiving = false;
        peer->snapshot_id = 0U;
    }
    return removed;
}

bool agent_peer_routes_snapshot_begin(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    uint64_t snapshot_id
)
{
    struct agent_peer *peer = mutable_peer(manager, peer_id);

    if (peer == NULL || snapshot_id == 0U ||
        peer->state != AGENT_PEER_STATE_ESTABLISHED) {
        return false;
    }
    peer->snapshot_receiving = true;
    peer->snapshot_id = snapshot_id;
    manager->snapshots_started++;
    return true;
}

enum agent_peer_route_result agent_peer_routes_snapshot_end(
    struct agent_peer_route_manager *manager,
    const char *peer_id,
    uint64_t snapshot_id,
    agent_peer_route_removed_handler removed_handler,
    void *removed_context
)
{
    struct agent_peer *peer = mutable_peer(manager, peer_id);
    struct agent_route *route;
    struct agent_route *next;
    size_t removed = 0U;

    if (peer == NULL || snapshot_id == 0U ||
        !peer->snapshot_receiving || peer->snapshot_id != snapshot_id) {
        return AGENT_PEER_ROUTE_INVALID;
    }
    route = manager->routes->head;
    while (route != NULL) {
        next = route->next;
        if (route->source == AGENT_ROUTE_SOURCE_PEER &&
            strcmp(route->learned_from_peer, peer_id) == 0 &&
            route->snapshot_id != snapshot_id) {
            if (removed_handler != NULL) {
                removed_handler(removed_context, route);
            }
            if (route_table_remove(manager->routes, route->route_id) !=
                ROUTE_TABLE_OK) {
                return AGENT_PEER_ROUTE_TABLE_ERROR;
            }
            removed++;
        }
        route = next;
    }
    peer->snapshot_receiving = false;
    peer->snapshot_id = 0U;
    peer->snapshots_completed++;
    manager->snapshots_completed++;
    manager->snapshot_routes_removed += removed;
    manager->routes_removed += removed;
    recount_peer(manager, peer);
    return AGENT_PEER_ROUTE_OK;
}

bool agent_peer_routes_make_update(
    const struct agent_route *route,
    const char *local_router_id,
    uint64_t now_ms,
    struct agent_arpx_message *message
)
{
    return agent_peer_routes_export_update(
        route, local_router_id, NULL, false, now_ms, message);
}

bool agent_peer_routes_export_update(
    const struct agent_route *route,
    const char *local_router_id,
    const struct agent_peer *target_peer,
    bool reflector_enabled,
    uint64_t now_ms,
    struct agent_arpx_message *message
)
{
    uint64_t remaining;
    size_t i;

    if (route == NULL || message == NULL ||
        !route->healthy || route->lease_expires_ms <= now_ms) {
        return false;
    }
    if (route->source == AGENT_ROUTE_SOURCE_PEER) {
        if (!reflector_enabled || target_peer == NULL ||
            strcmp(route->learned_from_peer, target_peer->peer_id) == 0 ||
            route->path_length == 0U ||
            route->path_length >= AGENT_ARPX_MAX_PATH ||
            route_path_contains(route, target_peer->router_id)) {
            return false;
        }
    } else if (route->source != AGENT_ROUTE_SOURCE_LOCAL &&
               route->source != AGENT_ROUTE_SOURCE_STATIC) {
        return false;
    }
    remaining = route->lease_expires_ms - now_ms;
    if (remaining > AGENT_ARPX_MAX_REMAINING_LEASE_MS) {
        remaining = AGENT_ARPX_MAX_REMAINING_LEASE_MS;
    }
    memset(message, 0, sizeof(*message));
    message->version = AGENT_ARPX_VERSION;
    message->type = AGENT_ARPX_CAPABILITY_UPDATE;
    if (!copy_text(message->route_id, sizeof(message->route_id),
                   route->route_id) ||
        !copy_text(message->intent, sizeof(message->intent), route->intent) ||
        !copy_text(message->origin, sizeof(message->origin), route->origin) ||
        !copy_text(message->endpoint, sizeof(message->endpoint),
                   route->endpoint) ||
        !copy_text(message->tenant, sizeof(message->tenant), route->tenant) ||
        !copy_text(message->region, sizeof(message->region), route->region)) {
        return false;
    }
    message->capability_version = route->version;
    message->cost_microunits = route->cost_microunits;
    message->latency_ms = route->latency_ms;
    message->trust_level = route->trust_level;
    message->load_permille = route->load_permille;
    message->remaining_lease_ms = (uint32_t)remaining;
    if (route->source == AGENT_ROUTE_SOURCE_PEER) {
        message->path_length = route->path_length + 1U;
        for (i = 0U; i < route->path_length; i++) {
            if (!copy_text(message->path[i], sizeof(message->path[i]),
                           route->path[i])) {
                return false;
            }
        }
        if (!copy_text(message->path[route->path_length],
                       sizeof(message->path[route->path_length]),
                       local_router_id)) {
            return false;
        }
    } else {
        message->path_length = 1U;
        if (!copy_text(message->path[0], sizeof(message->path[0]),
                       local_router_id)) {
            return false;
        }
    }
    return true;
}

bool agent_peer_routes_make_withdraw(
    const char *route_id,
    struct agent_arpx_message *message
)
{
    if (route_id == NULL || message == NULL) {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->version = AGENT_ARPX_VERSION;
    message->type = AGENT_ARPX_CAPABILITY_WITHDRAW;
    return copy_text(message->route_id, sizeof(message->route_id), route_id);
}

static bool make_snapshot_message(
    enum agent_arpx_message_type type,
    uint64_t snapshot_id,
    struct agent_arpx_message *message
)
{
    if (message == NULL || snapshot_id == 0U) {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->version = AGENT_ARPX_VERSION;
    message->type = type;
    message->snapshot_id = snapshot_id;
    return true;
}

bool agent_peer_routes_make_snapshot_request(
    uint64_t snapshot_id,
    struct agent_arpx_message *message
)
{
    return make_snapshot_message(
        AGENT_ARPX_SNAPSHOT_REQUEST, snapshot_id, message);
}

bool agent_peer_routes_make_snapshot_end(
    uint64_t snapshot_id,
    struct agent_arpx_message *message
)
{
    return make_snapshot_message(AGENT_ARPX_SNAPSHOT_END, snapshot_id,
                                 message);
}

#include "agent_peer_routes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool authorize_lint(
    void *context,
    const struct agent_peer *peer,
    const struct agent_arpx_message *message,
    uint64_t now_ms
)
{
    size_t *calls = context;

    (void)peer;
    (void)now_ms;
    (*calls)++;
    return strcmp(message->intent, "chip.lint.v1") == 0;
}

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);

    assert(written >= 0 && (size_t)written < capacity);
}

static struct agent_peer make_peer(void)
{
    struct agent_peer peer;

    memset(&peer, 0, sizeof(peer));
    set_text(peer.peer_id, sizeof(peer.peer_id), "peer-a");
    set_text(peer.router_id, sizeof(peer.router_id), "router-a");
    set_text(peer.domain_id, sizeof(peer.domain_id), "eda.example");
    set_text(peer.endpoint, sizeof(peer.endpoint),
             "https://router-a.eda.example:7444/arpx/v1");
    set_text(peer.connect_ipv4, sizeof(peer.connect_ipv4), "192.0.2.1");
    peer.role = AGENT_PEER_ROLE_DIRECT;
    peer.state = AGENT_PEER_STATE_CONFIGURED;
    peer.graceful_restart_seconds = 30U;
    return peer;
}

static struct agent_arpx_message make_update(const char *route_id)
{
    struct agent_arpx_message message;

    memset(&message, 0, sizeof(message));
    message.version = AGENT_ARPX_VERSION;
    message.type = AGENT_ARPX_CAPABILITY_UPDATE;
    set_text(message.router_id, sizeof(message.router_id), "router-a");
    set_text(message.domain_id, sizeof(message.domain_id), "eda.example");
    message.boot_epoch = 42U;
    message.sequence = 2U;
    set_text(message.route_id, sizeof(message.route_id), route_id);
    set_text(message.intent, sizeof(message.intent), "chip.lint.v1");
    message.capability_version = 1U;
    set_text(message.origin, sizeof(message.origin), "agent://lint-a");
    set_text(message.endpoint, sizeof(message.endpoint),
             "http://192.0.2.10:8080/invoke");
    set_text(message.tenant, sizeof(message.tenant), "eda");
    set_text(message.region, sizeof(message.region), "local");
    message.cost_microunits = 10U;
    message.latency_ms = 20U;
    message.trust_level = 90U;
    message.load_permille = 100U;
    message.remaining_lease_ms = 60000U;
    message.path_length = 1U;
    set_text(message.path[0], sizeof(message.path[0]), "router-a");
    return message;
}

static void setup(
    struct route_table *routes,
    struct peer_table *peers,
    struct agent_peer_route_manager *manager
)
{
    struct agent_peer peer = make_peer();

    route_table_init(routes, 16U);
    peer_table_init(peers, 4U);
    assert(peer_table_upsert(peers, &peer) == PEER_TABLE_OK);
    assert(agent_peer_routes_init(manager, "router-b", routes, peers));
}

static void test_update_withdraw_and_loop(void)
{
    const char *route_id = "0123456789abcdef0123456789abcdef";
    struct route_table routes;
    struct peer_table peers;
    struct agent_peer_route_manager manager;
    struct agent_arpx_message message = make_update(route_id);
    const struct agent_route *route;

    setup(&routes, &peers, &manager);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    route = route_table_find(&routes, route_id);
    assert(route != NULL && route->source == AGENT_ROUTE_SOURCE_PEER);
    assert(route->healthy && route->lease_expires_ms == 61000U);
    assert(strcmp(route->learned_from_peer, "peer-a") == 0);
    assert(peer_table_find(&peers, "peer-a")->learned_routes == 1U);

    message.sequence = 3U;
    message.path_length = 2U;
    set_text(message.path[0], sizeof(message.path[0]), "router-b");
    set_text(message.path[1], sizeof(message.path[1]), "router-a");
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 2000U) == AGENT_PEER_ROUTE_LOOP);

    message = make_update(route_id);
    message.type = AGENT_ARPX_CAPABILITY_WITHDRAW;
    memset(message.intent, 0, sizeof(message.intent));
    message.capability_version = 0U;
    memset(message.origin, 0, sizeof(message.origin));
    memset(message.endpoint, 0, sizeof(message.endpoint));
    memset(message.tenant, 0, sizeof(message.tenant));
    memset(message.region, 0, sizeof(message.region));
    message.path_length = 0U;
    message.remaining_lease_ms = 0U;
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 3000U) == AGENT_PEER_ROUTE_OK);
    assert(route_table_find(&routes, route_id) == NULL);
    assert(peer_table_find(&peers, "peer-a")->learned_routes == 0U);
    route_table_destroy(&routes);
    peer_table_destroy(&peers);
}

static void test_stale_and_graceful_removal(void)
{
    const char *route_id = "fedcba9876543210fedcba9876543210";
    struct route_table routes;
    struct peer_table peers;
    struct agent_peer_route_manager manager;
    struct agent_arpx_message message = make_update(route_id);
    const struct agent_route *route;
    const struct agent_peer *peer;

    setup(&routes, &peers, &manager);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    assert(peer_table_set_state(&peers, "peer-a", AGENT_PEER_STATE_CONNECTING)
           == PEER_TABLE_OK);
    assert(peer_table_set_state(&peers, "peer-a", AGENT_PEER_STATE_ESTABLISHED)
           == PEER_TABLE_OK);
    assert(agent_peer_routes_session_down(&manager, "peer-a", 2000U) == 1U);
    route = route_table_find(&routes, route_id);
    peer = peer_table_find(&peers, "peer-a");
    assert(route != NULL && !route->healthy);
    assert(route->lease_expires_ms == 32000U);
    assert(peer->state == AGENT_PEER_STATE_STALE);
    assert(agent_peer_routes_prune(&manager, 31999U) == 0U);
    assert(agent_peer_routes_prune(&manager, 32000U) == 1U);
    assert(route_table_find(&routes, route_id) == NULL);
    assert(peer_table_find(&peers, "peer-a")->state == AGENT_PEER_STATE_DOWN);
    route_table_destroy(&routes);
    peer_table_destroy(&peers);
}

static void test_open_mesh_tenant_includes_open_mesh_relay(void)
{
    const char *direct_id = "abababababababababababababababab";
    const char *relay_id = "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
    const char *mesh_relay_id = "dededededededededededededededede";
    struct route_table routes;
    struct peer_table peers;
    struct agent_peer_route_manager manager;
    struct agent_arpx_message message = make_update(direct_id);
    struct agent_peer relay = make_peer();
    const struct agent_route *route;

    setup(&routes, &peers, &manager);
    agent_peer_routes_set_open_mesh(&manager, true);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    route = route_table_find(&routes, direct_id);
    assert(route != NULL && strcmp(route->tenant, "*") == 0);

    assert(peer_table_remove(&peers, "peer-a") == PEER_TABLE_OK);
    relay.role = AGENT_PEER_ROLE_RELAY;
    assert(peer_table_upsert(&peers, &relay) == PEER_TABLE_OK);
    message = make_update(relay_id);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 2000U) == AGENT_PEER_ROUTE_OK);
    route = route_table_find(&routes, relay_id);
    assert(route != NULL && strcmp(route->tenant, "eda") == 0);

    relay.open_mesh = true;
    assert(peer_table_upsert(&peers, &relay) == PEER_TABLE_OK);
    message = make_update(mesh_relay_id);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 3000U) == AGENT_PEER_ROUTE_OK);
    route = route_table_find(&routes, mesh_relay_id);
    assert(route != NULL && strcmp(route->tenant, "*") == 0);

    route_table_destroy(&routes);
    peer_table_destroy(&peers);
}

static void test_explicit_peer_route_removal(void)
{
    const char *route_id = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    struct route_table routes;
    struct peer_table peers;
    struct agent_peer_route_manager manager;
    struct agent_arpx_message message = make_update(route_id);
    const struct agent_peer *peer;

    setup(&routes, &peers, &manager);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    assert(agent_peer_routes_remove_peer_routes(
               &manager, "missing-peer") == 0U);
    assert(agent_peer_routes_remove_peer_routes(&manager, "peer-a") == 1U);
    assert(route_table_find(&routes, route_id) == NULL);
    peer = peer_table_find(&peers, "peer-a");
    assert(peer != NULL && peer->learned_routes == 0U);
    assert(peer->stale_until_ms == 0U && !peer->snapshot_receiving);
    assert(manager.routes_removed == 1U);
    route_table_destroy(&routes);
    peer_table_destroy(&peers);
}

static void test_update_authorization_hook(void)
{
    struct route_table routes;
    struct peer_table peers;
    struct agent_peer_route_manager manager;
    struct agent_arpx_message message = make_update(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    size_t calls = 0U;

    setup(&routes, &peers, &manager);
    agent_peer_routes_set_authorizer(&manager, authorize_lint, &calls);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    set_text(message.route_id, sizeof(message.route_id),
             "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    set_text(message.intent, sizeof(message.intent), "chip.secret.v1");
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 2000U) ==
           AGENT_PEER_ROUTE_UNAUTHORIZED);
    assert(calls == 2U && manager.authorization_rejected == 1U);
    assert(route_table_find(
               &routes, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == NULL);
    route_table_destroy(&routes);
    peer_table_destroy(&peers);
}

static void test_export_contract(void)
{
    struct agent_route route;
    struct agent_arpx_message message;

    memset(&route, 0, sizeof(route));
    set_text(route.route_id, sizeof(route.route_id),
             "00112233445566778899aabbccddeeff");
    set_text(route.intent, sizeof(route.intent), "chip.lint.v1");
    route.version = 1U;
    set_text(route.origin, sizeof(route.origin), "agent://local");
    set_text(route.endpoint, sizeof(route.endpoint), "http://127.0.0.1:8080/");
    set_text(route.tenant, sizeof(route.tenant), "eda");
    set_text(route.region, sizeof(route.region), "local");
    route.trust_level = 100U;
    route.healthy = true;
    route.lease_expires_ms = 70000U;
    route.source = AGENT_ROUTE_SOURCE_LOCAL;
    assert(agent_peer_routes_make_update(
        &route, "router-b", 10000U, &message));
    assert(message.type == AGENT_ARPX_CAPABILITY_UPDATE);
    assert(message.remaining_lease_ms == 60000U);
    assert(message.path_length == 1U);
    assert(strcmp(message.path[0], "router-b") == 0);
    assert(agent_peer_routes_make_withdraw(route.route_id, &message));
    assert(message.type == AGENT_ARPX_CAPABILITY_WITHDRAW);
    assert(agent_peer_routes_make_snapshot_request(11U, &message));
    assert(message.type == AGENT_ARPX_SNAPSHOT_REQUEST);
    assert(message.snapshot_id == 11U);
    assert(agent_peer_routes_make_snapshot_end(11U, &message));
    assert(message.type == AGENT_ARPX_SNAPSHOT_END);
}

static void count_removed(void *context, const struct agent_route *route)
{
    size_t *count = context;

    assert(route != NULL);
    (*count)++;
}

static void test_snapshot_reconciliation(void)
{
    const char *kept_id = "11111111111111111111111111111111";
    const char *removed_id = "22222222222222222222222222222222";
    struct route_table routes;
    struct peer_table peers;
    struct agent_peer_route_manager manager;
    struct agent_arpx_message message = make_update(kept_id);
    size_t removed = 0U;

    setup(&routes, &peers, &manager);
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    message = make_update(removed_id);
    message.sequence = 3U;
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 1000U) == AGENT_PEER_ROUTE_OK);
    assert(peer_table_set_state(&peers, "peer-a", AGENT_PEER_STATE_CONNECTING)
           == PEER_TABLE_OK);
    assert(peer_table_set_state(&peers, "peer-a", AGENT_PEER_STATE_ESTABLISHED)
           == PEER_TABLE_OK);
    assert(agent_peer_routes_snapshot_begin(&manager, "peer-a", 99U));
    message = make_update(kept_id);
    message.sequence = 4U;
    assert(agent_peer_routes_on_message(
               &manager, "peer-a", &message, 2000U) == AGENT_PEER_ROUTE_OK);
    assert(route_table_find(&routes, kept_id)->snapshot_id == 99U);
    assert(agent_peer_routes_snapshot_end(
               &manager, "peer-a", 99U, count_removed, &removed) ==
           AGENT_PEER_ROUTE_OK);
    assert(removed == 1U);
    assert(route_table_find(&routes, kept_id) != NULL);
    assert(route_table_find(&routes, removed_id) == NULL);
    assert(manager.snapshots_started == 1U);
    assert(manager.snapshots_completed == 1U);
    assert(manager.snapshot_routes_removed == 1U);
    route_table_destroy(&routes);
    peer_table_destroy(&peers);
}

static void test_reflector_split_horizon(void)
{
    struct agent_route route;
    struct agent_peer target;
    struct agent_arpx_message message;

    memset(&route, 0, sizeof(route));
    set_text(route.route_id, sizeof(route.route_id),
             "33333333333333333333333333333333");
    set_text(route.intent, sizeof(route.intent), "chip.lint.v1");
    route.version = 1U;
    set_text(route.origin, sizeof(route.origin), "agent://remote");
    set_text(route.endpoint, sizeof(route.endpoint),
             "http://192.0.2.20:8080/invoke");
    set_text(route.tenant, sizeof(route.tenant), "eda");
    set_text(route.region, sizeof(route.region), "local");
    route.trust_level = 90U;
    route.healthy = true;
    route.lease_expires_ms = 70000U;
    route.source = AGENT_ROUTE_SOURCE_PEER;
    set_text(route.learned_from_peer, sizeof(route.learned_from_peer),
             "peer-a");
    route.path_length = 1U;
    set_text(route.path[0], sizeof(route.path[0]), "router-a");

    memset(&target, 0, sizeof(target));
    set_text(target.peer_id, sizeof(target.peer_id), "peer-c");
    set_text(target.router_id, sizeof(target.router_id), "router-c");
    assert(!agent_peer_routes_export_update(
        &route, "router-b", &target, false, 10000U, &message));
    assert(agent_peer_routes_export_update(
        &route, "router-b", &target, true, 10000U, &message));
    assert(message.path_length == 2U);
    assert(strcmp(message.path[0], "router-a") == 0);
    assert(strcmp(message.path[1], "router-b") == 0);

    set_text(target.peer_id, sizeof(target.peer_id), "peer-a");
    assert(!agent_peer_routes_export_update(
        &route, "router-b", &target, true, 10000U, &message));
    set_text(target.peer_id, sizeof(target.peer_id), "peer-c");
    set_text(target.router_id, sizeof(target.router_id), "router-a");
    assert(!agent_peer_routes_export_update(
        &route, "router-b", &target, true, 10000U, &message));
}

int main(void)
{
    test_update_withdraw_and_loop();
    test_stale_and_graceful_removal();
    test_open_mesh_tenant_includes_open_mesh_relay();
    test_explicit_peer_route_removal();
    test_update_authorization_hook();
    test_export_contract();
    test_snapshot_reconciliation();
    test_reflector_split_horizon();
    puts("peer route exchange tests passed");
    return 0;
}

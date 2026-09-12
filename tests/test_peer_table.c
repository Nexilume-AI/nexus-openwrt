#include "peer_table.h"
#include "agent_peer_transport_contract.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *value)
{
    int written = snprintf(target, capacity, "%s", value);

    assert(written >= 0);
    assert((size_t)written < capacity);
}

static struct agent_peer make_peer(
    const char *peer_id,
    const char *router_id,
    const char *endpoint
)
{
    struct agent_peer peer;

    memset(&peer, 0, sizeof(peer));
    set_text(peer.peer_id, sizeof(peer.peer_id), peer_id);
    set_text(peer.router_id, sizeof(peer.router_id), router_id);
    set_text(peer.domain_id, sizeof(peer.domain_id), "eda.example");
    set_text(peer.endpoint, sizeof(peer.endpoint), endpoint);
    peer.role = AGENT_PEER_ROLE_DIRECT;
    peer.state = AGENT_PEER_STATE_CONFIGURED;
    peer.graceful_restart_seconds = 30U;
    return peer;
}

static void test_contract_validation(void)
{
    struct agent_peer peer = make_peer(
        "edge-a", "router-a", "https://router-a.example:7444/arpx/v1");

    assert(agent_peer_valid(&peer));
    assert(agent_peer_id_valid("router-1_az", AGENT_ROUTER_ID_LEN));
    assert(!agent_peer_id_valid("Router-A", AGENT_ROUTER_ID_LEN));
    assert(!agent_peer_id_valid("-router", AGENT_ROUTER_ID_LEN));
    assert(agent_peer_domain_valid("eda.example"));
    assert(!agent_peer_domain_valid("EDA.example"));
    assert(!agent_peer_domain_valid("eda..example"));
    assert(agent_peer_endpoint_valid(
        "https://router-a.example:7444/arpx/v1"));
    assert(agent_peer_endpoint_valid(
        "https://192.0.2.10:7444/arpx/v1"));
    assert(agent_peer_transport_ipv4_valid("192.168.250.2"));
    assert(!agent_peer_transport_ipv4_valid("192.168.250.999"));
    assert(!agent_peer_endpoint_valid(
        "http://router-a.example:7444/arpx/v1"));
    assert(!agent_peer_endpoint_valid(
        "https://router-a.example/arpx/v1"));
    assert(!agent_peer_endpoint_valid(
        "https://user@router-a.example:7444/arpx/v1"));
    assert(!agent_peer_endpoint_valid(
        "https://router-a.example:7444/arpx/v2"));
    assert(!agent_peer_endpoint_valid(
        "https://router-a.example:999999999999999999999/arpx/v1"));
    peer.role = AGENT_PEER_ROLE_RELAY;
    assert(agent_peer_valid(&peer));
    assert(strcmp(agent_peer_role_name(peer.role), "relay") == 0);
}

static void test_capacity_and_duplicate_identity(void)
{
    struct peer_table table;
    struct agent_peer first = make_peer(
        "edge-a", "router-a", "https://router-a.example:7444/arpx/v1");
    struct agent_peer duplicate_router = make_peer(
        "edge-b", "router-a", "https://router-b.example:7444/arpx/v1");
    struct agent_peer second = make_peer(
        "edge-b", "router-b", "https://router-b.example:7444/arpx/v1");

    peer_table_init(&table, 1U);
    assert(peer_table_upsert(&table, &first) == PEER_TABLE_OK);
    assert(table.count == 1U);
    assert(peer_table_upsert(&table, &duplicate_router) ==
           PEER_TABLE_DUPLICATE);
    assert(peer_table_upsert(&table, &second) == PEER_TABLE_FULL);
    assert(table.count == 1U);
    peer_table_destroy(&table);
}

static void test_upsert_find_remove(void)
{
    struct peer_table table;
    struct agent_peer peer = make_peer(
        "edge-a", "router-a", "https://router-a.example:7444/arpx/v1");
    const struct agent_peer *found;

    peer_table_init(&table, 4U);
    assert(peer_table_upsert(&table, &peer) == PEER_TABLE_OK);
    assert(table.generation == 1U);
    found = peer_table_find(&table, "edge-a");
    assert(found != NULL);
    assert(strcmp(found->router_id, "router-a") == 0);

    peer.role = AGENT_PEER_ROLE_REFLECTOR;
    assert(peer_table_upsert(&table, &peer) == PEER_TABLE_OK);
    assert(table.count == 1U);
    assert(table.generation == 2U);
    assert(peer_table_find(&table, "edge-a")->role ==
           AGENT_PEER_ROLE_REFLECTOR);

    assert(peer_table_remove(&table, "missing") == PEER_TABLE_NOT_FOUND);
    assert(peer_table_remove(&table, "edge-a") == PEER_TABLE_OK);
    assert(table.count == 0U);
    assert(table.generation == 3U);
    peer_table_destroy(&table);
}

static void test_state_machine(void)
{
    struct peer_table table;
    struct agent_peer peer = make_peer(
        "edge-a", "router-a", "https://router-a.example:7444/arpx/v1");

    peer_table_init(&table, 4U);
    assert(peer_table_upsert(&table, &peer) == PEER_TABLE_OK);
    assert(peer_table_set_state(&table, "edge-a",
                                AGENT_PEER_STATE_ESTABLISHED) ==
           PEER_TABLE_INVALID_TRANSITION);
    assert(peer_table_set_state(&table, "edge-a",
                                AGENT_PEER_STATE_CONNECTING) == PEER_TABLE_OK);
    assert(peer_table_set_state(&table, "edge-a",
                                AGENT_PEER_STATE_ESTABLISHED) == PEER_TABLE_OK);
    assert(peer_table_set_state(&table, "edge-a",
                                AGENT_PEER_STATE_STALE) == PEER_TABLE_OK);
    assert(peer_table_set_state(&table, "edge-a",
                                AGENT_PEER_STATE_ESTABLISHED) == PEER_TABLE_OK);
    assert(peer_table_set_state(&table, "edge-a",
                                AGENT_PEER_STATE_DOWN) == PEER_TABLE_OK);
    assert(strcmp(agent_peer_state_name(
        peer_table_find(&table, "edge-a")->state), "down") == 0);
    assert(strcmp(agent_peer_role_name(AGENT_PEER_ROLE_DIRECT), "direct") == 0);
    peer_table_destroy(&table);
}

static void test_relay_ticket_refresh_preserves_runtime_state(void)
{
    struct peer_table table;
    struct agent_peer peer = make_peer(
        "relay-a", "router-relay-a",
        "https://relay-a.example:7444/arpx/v1");
    uint64_t generation;

    peer.role = AGENT_PEER_ROLE_RELAY;
    peer.state = AGENT_PEER_STATE_ESTABLISHED;
    set_text(peer.relay_session_ticket,
             sizeof(peer.relay_session_ticket), "ticket-old");
    peer_table_init(&table, 4U);
    assert(peer_table_upsert(&table, &peer) == PEER_TABLE_OK);
    generation = table.generation;

    assert(peer_table_set_relay_ticket(
               &table, "relay-a", "ticket-renewed") == PEER_TABLE_OK);
    assert(strcmp(peer_table_find(&table, "relay-a")->relay_session_ticket,
                  "ticket-renewed") == 0);
    assert(peer_table_find(&table, "relay-a")->state ==
           AGENT_PEER_STATE_ESTABLISHED);
    assert(table.generation == generation);
    assert(peer_table_set_relay_ticket(
               &table, "missing", "ticket") == PEER_TABLE_NOT_FOUND);
    assert(peer_table_set_relay_ticket(
               &table, "relay-a", "") == PEER_TABLE_INVALID);
    peer_table_destroy(&table);
}

static void test_swap(void)
{
    struct peer_table left;
    struct peer_table right;
    struct agent_peer peer = make_peer(
        "edge-a", "router-a", "https://router-a.example:7444/arpx/v1");

    peer_table_init(&left, 4U);
    peer_table_init(&right, 8U);
    assert(peer_table_upsert(&right, &peer) == PEER_TABLE_OK);
    right.generation = 9U;
    peer_table_swap(&left, &right);
    assert(left.count == 1U);
    assert(left.max_peers == 8U);
    assert(left.generation == 9U);
    assert(right.count == 0U);
    peer_table_destroy(&left);
    peer_table_destroy(&right);
}

int main(void)
{
    test_contract_validation();
    test_capacity_and_duplicate_identity();
    test_upsert_find_remove();
    test_state_machine();
    test_relay_ticket_refresh_preserves_runtime_state();
    test_swap();
    puts("peer table tests passed");
    return 0;
}

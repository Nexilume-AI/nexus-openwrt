#include "route_table.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t size, const char *value)
{
    int written = snprintf(target, size, "%s", value);
    assert(written >= 0);
    assert((size_t)written < size);
}

static struct agent_route make_route(
    const char *route_id,
    const char *tenant,
    const char *region,
    uint64_t cost,
    uint32_t latency,
    uint8_t trust,
    uint16_t load,
    uint64_t expires
)
{
    struct agent_route route;

    memset(&route, 0, sizeof(route));
    set_text(route.route_id, sizeof(route.route_id), route_id);
    set_text(route.intent, sizeof(route.intent),
             "chip.verilog.verify.lint");
    route.version = 1U;
    set_text(route.origin, sizeof(route.origin),
             "agent://tenant-a/lint");
    set_text(route.endpoint, sizeof(route.endpoint),
             "https://127.0.0.1:9001/invoke");
    set_text(route.tenant, sizeof(route.tenant), tenant);
    set_text(route.region, sizeof(route.region), region);
    route.cost_microunits = cost;
    route.latency_ms = latency;
    route.trust_level = trust;
    route.load_permille = load;
    route.hop_count = 0U;
    route.healthy = true;
    route.lease_expires_ms = expires;
    route.sequence = 1U;
    route.source = AGENT_ROUTE_SOURCE_LOCAL;
    return route;
}

static struct route_query make_query(const char *tenant)
{
    struct route_query query;

    memset(&query, 0, sizeof(query));
    set_text(query.intent, sizeof(query.intent),
             "chip.verilog.verify.lint");
    query.version = 1U;
    set_text(query.tenant, sizeof(query.tenant), tenant);
    set_text(query.region, sizeof(query.region), "local");
    query.min_trust_level = 70U;
    return query;
}

static void test_selection_and_constraints(void)
{
    struct route_table table;
    struct agent_route fast;
    struct agent_route cheap;
    struct route_query query;
    struct route_selection selection;

    route_table_init(&table, 8U);
    fast = make_route("00000000000000000000000000000001",
                      "tenant-a", "local", 10000U, 100U, 90U, 100U, 5000U);
    cheap = make_route("00000000000000000000000000000002",
                       "tenant-a", "local", 5000U, 200U, 95U, 100U, 5000U);

    assert(route_table_upsert(&table, &fast) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &cheap) == ROUTE_TABLE_OK);
    assert(table.count == 2U);

    query = make_query("tenant-a");
    assert(route_table_lookup(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->route_id, fast.route_id) == 0);
    assert(route_table_lookup_id(&table, &query, cheap.route_id, 1000U,
                                 &selection));
    assert(strcmp(selection.route->route_id, cheap.route_id) == 0);
    assert(!route_table_lookup_id(
        &table, &query, "000000000000000000000000000000ff", 1000U,
        &selection));

    query.max_cost_microunits = 6000U;
    assert(route_table_lookup(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->route_id, cheap.route_id) == 0);

    query.max_latency_ms = 150U;
    assert(!route_table_lookup(&table, &query, 1000U, &selection));
    route_table_destroy(&table);
}

static void test_exact_target_agent_selection(void)
{
    struct route_table table;
    struct agent_route best;
    struct agent_route target;
    struct route_query query;
    struct route_selection selection;
    struct route_diagnostics diagnostics;

    route_table_init(&table, 4U);
    best = make_route("00000000000000000000000000000051",
                      "tenant-a", "local", 10U, 5U, 100U, 0U, 5000U);
    target = make_route("00000000000000000000000000000052",
                        "tenant-a", "local", 1000U, 100U, 90U, 0U, 5000U);
    set_text(best.origin, sizeof(best.origin), "agent://tenant-a/linter-fast");
    set_text(target.origin, sizeof(target.origin),
             "agent://tenant-a/linter-exact");
    assert(route_table_upsert(&table, &best) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &target) == ROUTE_TABLE_OK);

    query = make_query("tenant-a");
    assert(route_table_lookup(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->origin,
                  "agent://tenant-a/linter-fast") == 0);

    set_text(query.target_agent, sizeof(query.target_agent),
             "agent://tenant-a/linter-exact");
    assert(route_table_lookup_ex(&table, &query, 1000U, &selection,
                                 &diagnostics));
    assert(strcmp(selection.route->origin,
                  "agent://tenant-a/linter-exact") == 0);
    assert(diagnostics.eligible == 1U);
    assert(diagnostics.target_agent_mismatch == 1U);

    set_text(query.target_agent, sizeof(query.target_agent),
             "agent://tenant-a/missing");
    assert(!route_table_lookup(&table, &query, 1000U, &selection));
    route_table_destroy(&table);
}

static void test_tenant_and_region_isolation(void)
{
    struct route_table table;
    struct agent_route private_route;
    struct agent_route shared_route;
    struct route_query query;
    struct route_selection selection;

    route_table_init(&table, 8U);
    private_route = make_route("00000000000000000000000000000003",
                               "tenant-a", "cn-east", 100U, 10U, 100U, 0U,
                               5000U);
    shared_route = make_route("00000000000000000000000000000004",
                              "*", "*", 1000U, 20U, 90U, 0U, 5000U);
    assert(route_table_upsert(&table, &private_route) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &shared_route) == ROUTE_TABLE_OK);

    query = make_query("tenant-b");
    set_text(query.region, sizeof(query.region), "eu-west");
    assert(route_table_lookup(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->route_id, shared_route.route_id) == 0);

    query = make_query("tenant-a");
    set_text(query.region, sizeof(query.region), "cn-east");
    assert(route_table_lookup(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->route_id, private_route.route_id) == 0);
    route_table_destroy(&table);
}

static void test_renewal_expiry_and_withdrawal(void)
{
    struct route_table table;
    struct agent_route route;
    struct route_query query;
    struct route_selection selection;
    struct route_renewal renewal;
    uint64_t generation;

    route_table_init(&table, 2U);
    route = make_route("00000000000000000000000000000005",
                       "tenant-a", "local", 100U, 10U, 90U, 0U, 2000U);
    assert(route_table_upsert(&table, &route) == ROUTE_TABLE_OK);
    generation = table.generation;

    memset(&renewal, 0, sizeof(renewal));
    renewal.lease_expires_ms = 4000U;
    renewal.update_health = true;
    renewal.healthy = false;
    assert(route_table_renew(&table, route.route_id, &renewal) ==
           ROUTE_TABLE_OK);
    assert(table.generation == generation + 1U);

    query = make_query("tenant-a");
    assert(!route_table_lookup(&table, &query, 1000U, &selection));

    renewal.lease_expires_ms = 4000U;
    renewal.update_health = true;
    renewal.healthy = true;
    renewal.update_latency = true;
    renewal.latency_ms = 9U;
    assert(route_table_renew(&table, route.route_id, &renewal) ==
           ROUTE_TABLE_OK);
    assert(route_table_lookup(&table, &query, 3000U, &selection));

    assert(route_table_prune_expired(&table, 4000U) == 1U);
    assert(table.count == 0U);
    assert(route_table_remove(&table, route.route_id) ==
           ROUTE_TABLE_NOT_FOUND);
    route_table_destroy(&table);
}

static void test_upsert_and_capacity(void)
{
    struct route_table table;
    struct agent_route first;
    struct agent_route replacement;
    struct agent_route overflow;

    route_table_init(&table, 1U);
    first = make_route("00000000000000000000000000000006",
                       "tenant-a", "local", 100U, 10U, 90U, 0U, 5000U);
    replacement = first;
    replacement.latency_ms = 5U;
    overflow = make_route("00000000000000000000000000000007",
                          "tenant-a", "local", 100U, 10U, 90U, 0U, 5000U);

    assert(route_table_upsert(&table, &first) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &replacement) == ROUTE_TABLE_OK);
    assert(table.count == 1U);
    assert(route_table_first(&table)->latency_ms == 5U);
    assert(route_table_upsert(&table, &overflow) == ROUTE_TABLE_FULL);
    route_table_destroy(&table);
}

static void test_diagnostics(void)
{
    struct route_table table;
    struct agent_route allowed;
    struct agent_route wrong_tenant;
    struct agent_route expired;
    struct route_query query;
    struct route_selection selection;
    struct route_diagnostics diagnostics;

    route_table_init(&table, 4U);
    allowed = make_route("00000000000000000000000000000008",
                         "tenant-a", "local", 100U, 10U, 90U, 0U, 5000U);
    wrong_tenant = make_route("00000000000000000000000000000009",
                              "tenant-b", "local", 100U, 10U, 90U, 0U,
                              5000U);
    expired = make_route("0000000000000000000000000000000a",
                         "tenant-a", "local", 100U, 10U, 90U, 0U, 500U);
    assert(route_table_upsert(&table, &allowed) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &wrong_tenant) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &expired) == ROUTE_TABLE_OK);

    query = make_query("tenant-a");
    assert(route_table_lookup_ex(&table, &query, 1000U, &selection,
                                 &diagnostics));
    assert(diagnostics.total == 3U);
    assert(diagnostics.eligible == 1U);
    assert(diagnostics.tenant_denied == 1U);
    assert(diagnostics.expired == 1U);
    route_table_destroy(&table);
}

static void test_ordered_candidates(void)
{
    struct route_table table;
    struct agent_route first;
    struct agent_route second;
    struct agent_route third;
    struct route_query query;
    struct route_selection selections[2];
    struct route_diagnostics diagnostics;

    route_table_init(&table, 4U);
    first = make_route("00000000000000000000000000000010",
                       "tenant-a", "local", 100U, 10U, 100U, 0U, 5000U);
    second = make_route("00000000000000000000000000000011",
                        "tenant-a", "local", 100U, 20U, 100U, 0U, 5000U);
    third = make_route("00000000000000000000000000000012",
                       "tenant-a", "local", 100U, 30U, 100U, 0U, 5000U);
    assert(route_table_upsert(&table, &third) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &first) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &second) == ROUTE_TABLE_OK);
    query = make_query("tenant-a");
    memset(selections, 0, sizeof(selections));
    assert(route_table_lookup_candidates_ex(
               &table, &query, 1000U, selections, 2U,
               &diagnostics) == 2U);
    assert(strcmp(selections[0].route->route_id, first.route_id) == 0);
    assert(strcmp(selections[1].route->route_id, second.route_id) == 0);
    assert(selections[0].score < selections[1].score);
    assert(diagnostics.eligible == 3U);
    route_table_destroy(&table);
}

static void test_clone_excluding_source(void)
{
    struct route_table source;
    struct route_table clone;
    struct agent_route dynamic;
    struct agent_route static_route;

    route_table_init(&source, 4U);
    dynamic = make_route("0000000000000000000000000000000b",
                         "tenant-a", "local", 100U, 10U, 90U, 0U, 5000U);
    static_route = make_route("0000000000000000000000000000000c",
                              "tenant-a", "local", 100U, 10U, 90U, 0U,
                              UINT64_MAX);
    static_route.source = AGENT_ROUTE_SOURCE_STATIC;
    assert(route_table_upsert(&source, &dynamic) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&source, &static_route) == ROUTE_TABLE_OK);

    assert(route_table_clone_excluding_source(
               &clone, &source, AGENT_ROUTE_SOURCE_STATIC) == ROUTE_TABLE_OK);
    assert(clone.count == 1U);
    assert(route_table_find(&clone, dynamic.route_id) != NULL);
    assert(route_table_find(&clone, static_route.route_id) == NULL);
    assert(clone.generation == source.generation);

    route_table_swap(&source, &clone);
    assert(source.count == 1U);
    assert(clone.count == 2U);
    route_table_destroy(&source);
    route_table_destroy(&clone);
}

static void test_local_only_lookup(void)
{
    struct route_table table;
    struct agent_route local;
    struct agent_route peer;
    struct route_query query;
    struct route_selection selection;

    route_table_init(&table, 4U);
    local = make_route("00000000000000000000000000000021",
                       "tenant-a", "local", 1000U, 100U, 90U, 0U, 5000U);
    peer = make_route("00000000000000000000000000000022",
                      "tenant-a", "local", 1U, 1U, 100U, 0U, 5000U);
    peer.source = AGENT_ROUTE_SOURCE_PEER;
    peer.hop_count = 1U;
    set_text(peer.learned_from_peer, sizeof(peer.learned_from_peer),
             "relay-east");
    peer.path_length = 1U;
    set_text(peer.path[0], sizeof(peer.path[0]), "router-remote");
    assert(route_table_upsert(&table, &local) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &peer) == ROUTE_TABLE_OK);
    query = make_query("tenant-a");
    assert(route_table_lookup(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->route_id, peer.route_id) == 0);
    assert(route_table_lookup_local(&table, &query, 1000U, &selection));
    assert(strcmp(selection.route->route_id, local.route_id) == 0);
    route_table_destroy(&table);
}

static void test_peer_target_lookup(void)
{
    struct route_table table;
    struct agent_route via_b;
    struct agent_route via_d;
    struct agent_route wrong_target;
    struct route_query query;
    struct route_selection selection;

    route_table_init(&table, 6U);
    via_b = make_route("00000000000000000000000000000031",
                       "tenant-a", "local", 100U, 20U, 95U, 0U, 5000U);
    via_b.source = AGENT_ROUTE_SOURCE_PEER;
    via_b.hop_count = 2U;
    set_text(via_b.learned_from_peer, sizeof(via_b.learned_from_peer),
             "router-b");
    via_b.path_length = 2U;
    set_text(via_b.path[0], sizeof(via_b.path[0]), "router-c");
    set_text(via_b.path[1], sizeof(via_b.path[1]), "router-b");

    via_d = via_b;
    set_text(via_d.route_id, sizeof(via_d.route_id),
             "00000000000000000000000000000032");
    set_text(via_d.learned_from_peer, sizeof(via_d.learned_from_peer),
             "router-d");
    via_d.latency_ms = 40U;

    wrong_target = via_b;
    set_text(wrong_target.route_id, sizeof(wrong_target.route_id),
             "00000000000000000000000000000033");
    set_text(wrong_target.learned_from_peer,
             sizeof(wrong_target.learned_from_peer), "router-e");
    set_text(wrong_target.path[0], sizeof(wrong_target.path[0]),
             "router-z");
    wrong_target.latency_ms = 1U;

    assert(route_table_upsert(&table, &via_b) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &via_d) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&table, &wrong_target) == ROUTE_TABLE_OK);
    query = make_query("tenant-a");

    assert(route_table_lookup_peer_target(
        &table, &query, "router-c", NULL, 1000U, &selection));
    assert(strcmp(selection.route->learned_from_peer, "router-b") == 0);
    assert(route_table_lookup_peer_target(
        &table, &query, "router-c", "router-b", 1000U, &selection));
    assert(strcmp(selection.route->learned_from_peer, "router-d") == 0);
    assert(!route_table_lookup_peer_target(
        &table, &query, "router-missing", NULL, 1000U, &selection));

    via_d.healthy = false;
    assert(route_table_upsert(&table, &via_d) == ROUTE_TABLE_OK);
    assert(!route_table_lookup_peer_target(
        &table, &query, "router-c", "router-b", 1000U, &selection));
    route_table_destroy(&table);
}

static void test_index_consistency_and_memory(void)
{
    struct route_table table;
    struct agent_route route;
    struct route_query query;
    struct route_selection selection;
    struct route_renewal renewal = {0};
    size_t empty_bytes;

    route_table_init(&table, 10000U);
    assert(route_table_index_enabled(&table));
    assert(route_table_index_buckets(&table) >= 10000U);
    empty_bytes = route_table_memory_bytes(&table);
    assert(empty_bytes >= sizeof(table));

    route = make_route("00000000000000000000000000000041",
                       "tenant-a", "local", 100U, 10U, 90U, 0U, 5000U);
    set_text(route.intent, sizeof(route.intent), "chip.p72.before");
    assert(route_table_upsert(&table, &route) == ROUTE_TABLE_OK);
    assert(route_table_find(&table, route.route_id) != NULL);

    memset(&query, 0, sizeof(query));
    set_text(query.intent, sizeof(query.intent), "chip.p72.before");
    query.version = 1U;
    set_text(query.tenant, sizeof(query.tenant), "tenant-a");
    set_text(query.region, sizeof(query.region), "local");
    assert(route_table_lookup(&table, &query, 1000U, &selection));

    set_text(route.intent, sizeof(route.intent), "chip.p72.after");
    assert(route_table_upsert(&table, &route) == ROUTE_TABLE_OK);
    assert(!route_table_lookup(&table, &query, 1000U, &selection));
    set_text(query.intent, sizeof(query.intent), "chip.p72.after");
    assert(route_table_lookup(&table, &query, 1000U, &selection));

    renewal.lease_expires_ms = 6000U;
    renewal.update_latency = true;
    renewal.latency_ms = 7U;
    assert(route_table_renew(&table, route.route_id, &renewal) ==
           ROUTE_TABLE_OK);
    assert(route_table_find(&table, route.route_id)->latency_ms == 7U);
    assert(route_table_memory_bytes(&table) ==
           empty_bytes + sizeof(struct agent_route));
    assert(route_table_remove(&table, route.route_id) == ROUTE_TABLE_OK);
    assert(route_table_find(&table, route.route_id) == NULL);
    assert(route_table_memory_bytes(&table) == empty_bytes);
    route_table_destroy(&table);
}

int main(void)
{
    test_selection_and_constraints();
    test_exact_target_agent_selection();
    test_tenant_and_region_isolation();
    test_renewal_expiry_and_withdrawal();
    test_upsert_and_capacity();
    test_diagnostics();
    test_ordered_candidates();
    test_clone_excluding_source();
    test_local_only_lookup();
    test_peer_target_lookup();
    test_index_consistency_and_memory();
    puts("route_table tests passed");
    return 0;
}

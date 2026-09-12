#include "agent_local_agents.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void add_route(
    struct route_table *table,
    const char *id,
    const char *origin,
    const char *endpoint,
    const char *tenant,
    const char *intent,
    uint64_t expires,
    bool healthy,
    enum agent_route_source source
)
{
    struct agent_route route;

    memset(&route, 0, sizeof(route));
    (void)strncpy(route.route_id, id, sizeof(route.route_id) - 1U);
    (void)strncpy(route.origin, origin, sizeof(route.origin) - 1U);
    (void)strncpy(route.endpoint, endpoint, sizeof(route.endpoint) - 1U);
    (void)strncpy(route.tenant, tenant, sizeof(route.tenant) - 1U);
    (void)strncpy(route.intent, intent, sizeof(route.intent) - 1U);
    (void)strncpy(route.region, "local", sizeof(route.region) - 1U);
    route.version = 1U;
    route.lease_expires_ms = expires;
    route.healthy = healthy;
    route.source = source;
    if (source == AGENT_ROUTE_SOURCE_PEER) {
        (void)strncpy(route.learned_from_peer, "peer-a",
                      sizeof(route.learned_from_peer) - 1U);
        (void)strncpy(route.path[0], "router-a",
                      sizeof(route.path[0]) - 1U);
        route.path_length = 1U;
    }
    assert(route_table_upsert(table, &route) == ROUTE_TABLE_OK);
}

int main(void)
{
    struct route_table table;
    struct agent_local_summary summaries[2];
    const struct agent_local_summary *lint;
    struct agent_local_collection collection;

    route_table_init(&table, 16U);
    add_route(&table, "r1", "agent://lint", "http://10.0.0.2:8000", "acme",
              "chip.lint", 20000U, true, AGENT_ROUTE_SOURCE_LOCAL);
    add_route(&table, "r2", "agent://lint", "http://10.0.0.2:8000", "acme",
              "chip.fix", 15000U, false, AGENT_ROUTE_SOURCE_LOCAL);
    add_route(&table, "r3", "agent://expired", "http://10.0.0.3:8000", "acme",
              "chip.old", 9000U, true, AGENT_ROUTE_SOURCE_LOCAL);
    add_route(&table, "r4", "agent://remote", "https://peer/agent", "acme",
              "chip.remote", 30000U, true, AGENT_ROUTE_SOURCE_PEER);
    add_route(&table, "r5", "agent://sim", "http://10.0.0.4:8000", "acme",
              "chip.sim", 25000U, true, AGENT_ROUTE_SOURCE_LOCAL);

    collection = agent_local_agents_collect(&table, 10000U, summaries, 2U);
    assert(collection.count == 2U);
    assert(!collection.truncated);
    lint = strcmp(summaries[0].agent_id, "agent://lint") == 0
        ? &summaries[0] : &summaries[1];
    assert(strcmp(lint->agent_id, "agent://lint") == 0);
    assert(lint->capability_count == 2U);
    assert(lint->healthy_capability_count == 1U);
    assert(lint->earliest_expires_ms == 15000U);
    assert(lint->latest_expires_ms == 20000U);

    collection = agent_local_agents_collect(&table, 10000U, summaries, 1U);
    assert(collection.count == 1U);
    assert(collection.truncated);

    route_table_destroy(&table);
    puts("agent local summary tests passed");
    return 0;
}

#include "agent_policy.h"
#include "route_table.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void text(char *target, size_t capacity, const char *value)
{
    int written = snprintf(target, capacity, "%s", value);
    assert(written >= 0 && (size_t)written < capacity);
}

static struct agent_route make_route(const char *id, const char *peer,
                                     uint32_t latency, uint16_t load)
{
    struct agent_route route;
    memset(&route, 0, sizeof(route));
    text(route.route_id, sizeof(route.route_id), id);
    text(route.intent, sizeof(route.intent), "chip.verify.v1");
    route.version = 1U;
    text(route.origin, sizeof(route.origin), "agent://tenant-a/worker");
    text(route.endpoint, sizeof(route.endpoint), "https://192.0.2.10/invoke");
    text(route.tenant, sizeof(route.tenant), "tenant-a");
    text(route.region, sizeof(route.region), "local");
    route.cost_microunits = 10000U;
    route.latency_ms = latency;
    route.trust_level = 90U;
    route.load_permille = load;
    route.hop_count = 1U;
    route.healthy = true;
    route.lease_expires_ms = 100000U;
    route.sequence = 1U;
    if (peer == NULL) {
        route.source = AGENT_ROUTE_SOURCE_LOCAL;
    } else {
        route.source = AGENT_ROUTE_SOURCE_PEER;
        text(route.learned_from_peer, sizeof(route.learned_from_peer), peer);
        route.path_length = 1U;
        text(route.path[0], sizeof(route.path[0]), "router-remote");
    }
    return route;
}

static struct route_query make_query(const char *source_agent)
{
    struct route_query query;
    memset(&query, 0, sizeof(query));
    text(query.intent, sizeof(query.intent), "chip.verify.v1");
    query.version = 1U;
    text(query.tenant, sizeof(query.tenant), "tenant-a");
    text(query.source_agent, sizeof(query.source_agent), source_agent);
    return query;
}

static struct agent_policy_rule make_rule(const char *id, uint32_t priority)
{
    struct agent_policy_rule rule;
    memset(&rule, 0, sizeof(rule));
    text(rule.policy_id, sizeof(rule.policy_id), id);
    rule.priority = priority;
    rule.action = AGENT_POLICY_ALLOW;
    text(rule.match_tenant, sizeof(rule.match_tenant), "tenant-a");
    text(rule.match_source_agent, sizeof(rule.match_source_agent), "*");
    text(rule.match_intent, sizeof(rule.match_intent), "chip.verify.v1");
    return rule;
}

static void test_priority_and_explicit_deny(void)
{
    struct agent_policy_table policy;
    struct agent_policy_rule allow = make_rule("allow-general", 100U);
    struct agent_policy_rule deny = make_rule("deny-bot", 500U);
    struct agent_policy_decision decision;
    struct route_query query = make_query("agent://tenant-a/bot");

    deny.action = AGENT_POLICY_DENY;
    text(deny.match_source_agent, sizeof(deny.match_source_agent),
         "agent://tenant-a/bot");
    agent_policy_table_init(&policy);
    assert(agent_policy_table_add(&policy, &allow));
    assert(agent_policy_table_add(&policy, &deny));
    agent_policy_select(&policy, &query, &decision);
    assert(decision.matched && !decision.allow);
    assert(strcmp(decision.rule->policy_id, "deny-bot") == 0);
}

static void test_constraints_and_preferred_peer(void)
{
    struct route_table routes;
    struct agent_policy_table policy;
    struct agent_policy_rule rule = make_rule("tenant-te", 200U);
    struct agent_route preferred = make_route(
        "00000000000000000000000000000001", "peer-west", 100U, 100U);
    struct agent_route faster = make_route(
        "00000000000000000000000000000002", "peer-east", 20U, 100U);
    struct route_query query = make_query("agent://tenant-a/client");
    struct route_selection selection;
    struct route_diagnostics diagnostics;

    text(rule.preferred_peer, sizeof(rule.preferred_peer), "peer-west");
    rule.nonpreferred_peer_penalty = 500000U;
    rule.max_load_permille = 500U;
    text(rule.allowed_endpoint_prefix,
         sizeof(rule.allowed_endpoint_prefix), "https://");
    rule.tenant_max_inflight = 8U;
    rule.tenant_rate_per_second = 20U;
    rule.tenant_rate_burst = 40U;
    rule.source_mask = (uint8_t)(1U << AGENT_ROUTE_SOURCE_PEER);
    agent_policy_table_init(&policy);
    assert(agent_policy_table_add(&policy, &rule));
    route_table_init(&routes, 8U);
    route_table_set_policy(&routes, &policy);
    assert(route_table_upsert(&routes, &preferred) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&routes, &faster) == ROUTE_TABLE_OK);
    assert(route_table_lookup_ex(&routes, &query, 1U, &selection,
                                 &diagnostics));
    assert(strcmp(selection.route->learned_from_peer, "peer-west") == 0);
    assert(diagnostics.policy_matched && diagnostics.policy_allowed);
    assert(strcmp(diagnostics.policy_id, "tenant-te") == 0);

    preferred.load_permille = 900U;
    assert(route_table_upsert(&routes, &preferred) == ROUTE_TABLE_OK);
    assert(route_table_lookup(&routes, &query, 1U, &selection));
    assert(strcmp(selection.route->learned_from_peer, "peer-east") == 0);
    route_table_destroy(&routes);
}

static void test_policy_applies_to_local_and_transit(void)
{
    struct route_table routes;
    struct agent_policy_table policy;
    struct agent_policy_rule rule = make_rule("peer-only", 100U);
    struct agent_route local = make_route(
        "00000000000000000000000000000003", NULL, 10U, 10U);
    struct agent_route peer = make_route(
        "00000000000000000000000000000004", "peer-north", 20U, 10U);
    struct route_query query = make_query("agent://tenant-a/client");
    struct route_selection selection;

    rule.source_mask = (uint8_t)(1U << AGENT_ROUTE_SOURCE_PEER);
    agent_policy_table_init(&policy);
    assert(agent_policy_table_add(&policy, &rule));
    route_table_init(&routes, 8U);
    route_table_set_policy(&routes, &policy);
    assert(route_table_upsert(&routes, &local) == ROUTE_TABLE_OK);
    assert(route_table_upsert(&routes, &peer) == ROUTE_TABLE_OK);
    assert(!route_table_lookup_local(&routes, &query, 1U, &selection));
    assert(route_table_lookup_peer_target(&routes, &query, "router-remote",
                                          NULL, 1U, &selection));
    assert(strcmp(selection.route->route_id, peer.route_id) == 0);
    route_table_destroy(&routes);
}

static void test_health_dampening(void)
{
    struct route_table routes;
    struct agent_policy_table policy;
    struct agent_route route = make_route(
        "00000000000000000000000000000005", NULL, 10U, 10U);
    struct route_renewal renewal;
    const struct agent_route *stored;

    agent_policy_table_init(&policy);
    policy.health_failure_threshold = 3U;
    policy.health_recovery_threshold = 2U;
    route_table_init(&routes, 4U);
    route_table_set_policy(&routes, &policy);
    assert(route_table_upsert(&routes, &route) == ROUTE_TABLE_OK);
    memset(&renewal, 0, sizeof(renewal));
    renewal.lease_expires_ms = 100001U;
    renewal.update_health = true;
    renewal.healthy = false;
    assert(route_table_renew(&routes, route.route_id, &renewal) == ROUTE_TABLE_OK);
    assert(route_table_renew(&routes, route.route_id, &renewal) == ROUTE_TABLE_OK);
    stored = route_table_find(&routes, route.route_id);
    assert(stored != NULL && stored->healthy && stored->health_failure_streak == 2U);
    assert(route_table_renew(&routes, route.route_id, &renewal) == ROUTE_TABLE_OK);
    assert(!route_table_find(&routes, route.route_id)->healthy);
    renewal.healthy = true;
    assert(route_table_renew(&routes, route.route_id, &renewal) == ROUTE_TABLE_OK);
    assert(!route_table_find(&routes, route.route_id)->healthy);
    assert(route_table_renew(&routes, route.route_id, &renewal) == ROUTE_TABLE_OK);
    assert(route_table_find(&routes, route.route_id)->healthy);
    route_table_destroy(&routes);
}

int main(void)
{
    test_priority_and_explicit_deny();
    test_constraints_and_preferred_peer();
    test_policy_applies_to_local_and_transit();
    test_health_dampening();
    puts("agent policy tests passed");
    return 0;
}

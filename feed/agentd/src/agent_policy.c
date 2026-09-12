#include "agent_policy.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_LATENCY_WEIGHT 1000U
#define DEFAULT_COST_DIVISOR 100U
#define DEFAULT_LOAD_WEIGHT 100U
#define DEFAULT_TRUST_WEIGHT 5000U
#define DEFAULT_HOP_WEIGHT 10000U

static bool bounded(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool match_value(const char *pattern, const char *value)
{
    return pattern[0] == '\0' || strcmp(pattern, "*") == 0 ||
           strcmp(pattern, value) == 0;
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t saturating_multiply(uint64_t left, uint64_t right)
{
    return left != 0U && right > UINT64_MAX / left
        ? UINT64_MAX : left * right;
}

void agent_policy_table_init(struct agent_policy_table *table)
{
    if (table == NULL) return;
    memset(table, 0, sizeof(*table));
    table->generation = 1U;
    table->default_allow = true;
    table->health_failure_threshold = 1U;
    table->health_recovery_threshold = 1U;
}

bool agent_policy_table_add(struct agent_policy_table *table,
                            const struct agent_policy_rule *rule)
{
    struct agent_policy_rule normalized;
    size_t i;

    if (table == NULL || rule == NULL || table->count >= AGENT_POLICY_MAX_RULES ||
        !bounded(rule->policy_id, sizeof(rule->policy_id)) ||
        rule->policy_id[0] == '\0' ||
        !bounded(rule->match_tenant, sizeof(rule->match_tenant)) ||
        !bounded(rule->match_source_agent, sizeof(rule->match_source_agent)) ||
        !bounded(rule->match_intent, sizeof(rule->match_intent)) ||
        !bounded(rule->required_region, sizeof(rule->required_region)) ||
        !bounded(rule->required_peer, sizeof(rule->required_peer)) ||
        !bounded(rule->denied_peer, sizeof(rule->denied_peer)) ||
        !bounded(rule->preferred_peer, sizeof(rule->preferred_peer)) ||
        !bounded(rule->allowed_endpoint_prefix,
                 sizeof(rule->allowed_endpoint_prefix)) ||
        rule->action > AGENT_POLICY_DENY ||
        rule->min_trust_level > 100U ||
        rule->max_load_permille > 1000U ||
        (rule->source_mask & ~AGENT_POLICY_SOURCE_MASK_ALL) != 0U) {
        return false;
    }
    for (i = 0U; i < table->count; i++) {
        if (strcmp(table->rules[i].policy_id, rule->policy_id) == 0) return false;
    }
    normalized = *rule;
    if (normalized.match_tenant[0] == '\0') memcpy(normalized.match_tenant, "*", 2U);
    if (normalized.match_source_agent[0] == '\0') memcpy(normalized.match_source_agent, "*", 2U);
    if (normalized.match_intent[0] == '\0') memcpy(normalized.match_intent, "*", 2U);
    if (normalized.source_mask == 0U) normalized.source_mask = AGENT_POLICY_SOURCE_MASK_ALL;
    if (normalized.latency_weight == 0U) normalized.latency_weight = DEFAULT_LATENCY_WEIGHT;
    if (normalized.cost_divisor == 0U) normalized.cost_divisor = DEFAULT_COST_DIVISOR;
    if (normalized.load_weight == 0U) normalized.load_weight = DEFAULT_LOAD_WEIGHT;
    if (normalized.trust_weight == 0U) normalized.trust_weight = DEFAULT_TRUST_WEIGHT;
    if (normalized.hop_weight == 0U) normalized.hop_weight = DEFAULT_HOP_WEIGHT;
    table->rules[table->count++] = normalized;
    return true;
}

void agent_policy_select(const struct agent_policy_table *table,
                         const struct route_query *query,
                         struct agent_policy_decision *decision)
{
    const struct agent_policy_rule *candidate;
    size_t i;

    if (decision == NULL) return;
    memset(decision, 0, sizeof(*decision));
    decision->allow = table == NULL || table->default_allow;
    if (table == NULL || query == NULL) return;
    for (i = 0U; i < table->count; i++) {
        candidate = &table->rules[i];
        if (!match_value(candidate->match_tenant, query->tenant) ||
            !match_value(candidate->match_source_agent, query->source_agent) ||
            !match_value(candidate->match_intent, query->intent)) continue;
        if (decision->rule == NULL ||
            candidate->priority > decision->rule->priority ||
            (candidate->priority == decision->rule->priority &&
             strcmp(candidate->policy_id, decision->rule->policy_id) < 0)) {
            decision->rule = candidate;
        }
    }
    if (decision->rule != NULL) {
        decision->matched = true;
        decision->allow = decision->rule->action == AGENT_POLICY_ALLOW;
    }
}

enum agent_policy_rejection agent_policy_route_rejection(
    const struct agent_policy_decision *decision,
    const struct agent_route *route)
{
    const struct agent_policy_rule *rule;
    uint8_t source_bit;

    if (route == NULL || decision == NULL || !decision->allow)
        return AGENT_POLICY_REJECT_ACTION;
    rule = decision->rule;
    if (rule == NULL) return AGENT_POLICY_ROUTE_ALLOWED;
    if (rule->required_region[0] != '\0' && strcmp(rule->required_region, "*") != 0 &&
        strcmp(rule->required_region, route->region) != 0) return AGENT_POLICY_REJECT_REGION;
    if (rule->max_cost_microunits != 0U && route->cost_microunits > rule->max_cost_microunits)
        return AGENT_POLICY_REJECT_COST;
    if (rule->max_latency_ms != 0U && route->latency_ms > rule->max_latency_ms)
        return AGENT_POLICY_REJECT_LATENCY;
    if (route->trust_level < rule->min_trust_level) return AGENT_POLICY_REJECT_TRUST;
    if (rule->max_load_permille != 0U && route->load_permille > rule->max_load_permille)
        return AGENT_POLICY_REJECT_LOAD;
    if (rule->max_hops != 0U && route->hop_count > rule->max_hops)
        return AGENT_POLICY_REJECT_HOPS;
    source_bit = (uint8_t)(1U << (unsigned int)route->source);
    if ((rule->source_mask & source_bit) == 0U) return AGENT_POLICY_REJECT_SOURCE;
    if (rule->required_peer[0] != '\0' && strcmp(route->learned_from_peer, rule->required_peer) != 0)
        return AGENT_POLICY_REJECT_PEER;
    if (rule->denied_peer[0] != '\0' && strcmp(route->learned_from_peer, rule->denied_peer) == 0)
        return AGENT_POLICY_REJECT_PEER;
    if (rule->allowed_endpoint_prefix[0] != '\0' &&
        strncmp(route->endpoint, rule->allowed_endpoint_prefix,
                strlen(rule->allowed_endpoint_prefix)) != 0)
        return AGENT_POLICY_REJECT_PEER;
    return AGENT_POLICY_ROUTE_ALLOWED;
}

uint64_t agent_policy_route_score(const struct agent_policy_decision *decision,
                                  const struct agent_route *route)
{
    const struct agent_policy_rule *rule;
    uint64_t score = 0U;
    uint64_t trust_penalty;

    if (route == NULL) return UINT64_MAX;
    rule = decision == NULL ? NULL : decision->rule;
    if (rule == NULL) return agent_route_score(route);
    trust_penalty = (uint64_t)(100U - route->trust_level);
    score = saturating_add(score, saturating_multiply(route->latency_ms, rule->latency_weight));
    score = saturating_add(score, route->cost_microunits / rule->cost_divisor);
    score = saturating_add(score, saturating_multiply(route->load_permille, rule->load_weight));
    score = saturating_add(score, saturating_multiply(trust_penalty, rule->trust_weight));
    score = saturating_add(score, saturating_multiply(route->hop_count, rule->hop_weight));
    if (rule->preferred_peer[0] != '\0' && strcmp(route->learned_from_peer, rule->preferred_peer) != 0)
        score = saturating_add(score, rule->nonpreferred_peer_penalty);
    return score;
}

const char *agent_policy_action_name(enum agent_policy_action action)
{
    return action == AGENT_POLICY_DENY ? "deny" : "allow";
}

void agent_policy_get_admission(
    const struct agent_policy_decision *decision,
    struct agent_policy_admission *admission)
{
    if (admission == NULL) return;
    memset(admission, 0, sizeof(*admission));
    if (decision == NULL || !decision->allow || decision->rule == NULL) return;
    (void)snprintf(admission->policy_id, sizeof(admission->policy_id), "%s",
                   decision->rule->policy_id);
    admission->max_inflight = decision->rule->tenant_max_inflight;
    admission->rate_per_second = decision->rule->tenant_rate_per_second;
    admission->rate_burst = decision->rule->tenant_rate_burst;
}

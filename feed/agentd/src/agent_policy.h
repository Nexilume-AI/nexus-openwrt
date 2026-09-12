#ifndef NEXUS_AGENT_POLICY_H
#define NEXUS_AGENT_POLICY_H

#include "route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_POLICY_MAX_RULES 64U
#define AGENT_POLICY_ID_LEN 65U
#define AGENT_POLICY_AGENT_ID_LEN 256U
#define AGENT_POLICY_SOURCE_MASK_ALL 0x0fU

enum agent_policy_action {
    AGENT_POLICY_ALLOW = 0,
    AGENT_POLICY_DENY = 1
};

enum agent_policy_rejection {
    AGENT_POLICY_ROUTE_ALLOWED = 0,
    AGENT_POLICY_REJECT_ACTION,
    AGENT_POLICY_REJECT_REGION,
    AGENT_POLICY_REJECT_COST,
    AGENT_POLICY_REJECT_LATENCY,
    AGENT_POLICY_REJECT_TRUST,
    AGENT_POLICY_REJECT_LOAD,
    AGENT_POLICY_REJECT_HOPS,
    AGENT_POLICY_REJECT_SOURCE,
    AGENT_POLICY_REJECT_PEER
};

struct agent_policy_rule {
    char policy_id[AGENT_POLICY_ID_LEN];
    uint32_t priority;
    enum agent_policy_action action;
    char match_tenant[AGENT_TENANT_LEN];
    char match_source_agent[AGENT_POLICY_AGENT_ID_LEN];
    char match_intent[AGENT_INTENT_LEN];
    char required_region[AGENT_REGION_LEN];
    uint64_t max_cost_microunits;
    uint32_t max_latency_ms;
    uint8_t min_trust_level;
    uint16_t max_load_permille;
    uint8_t max_hops;
    uint8_t source_mask;
    char required_peer[AGENT_ROUTE_PEER_ID_LEN];
    char denied_peer[AGENT_ROUTE_PEER_ID_LEN];
    char preferred_peer[AGENT_ROUTE_PEER_ID_LEN];
    char allowed_endpoint_prefix[AGENT_URI_LEN];
    uint64_t nonpreferred_peer_penalty;
    uint32_t tenant_max_inflight;
    uint32_t tenant_rate_per_second;
    uint32_t tenant_rate_burst;
    uint32_t latency_weight;
    uint32_t cost_divisor;
    uint32_t load_weight;
    uint32_t trust_weight;
    uint32_t hop_weight;
};

struct agent_policy_table {
    struct agent_policy_rule rules[AGENT_POLICY_MAX_RULES];
    size_t count;
    uint64_t generation;
    bool default_allow;
    uint8_t health_failure_threshold;
    uint8_t health_recovery_threshold;
};

struct agent_policy_decision {
    bool matched;
    bool allow;
    const struct agent_policy_rule *rule;
};

struct agent_policy_admission {
    char policy_id[AGENT_POLICY_ID_LEN];
    uint32_t max_inflight;
    uint32_t rate_per_second;
    uint32_t rate_burst;
};

void agent_policy_table_init(struct agent_policy_table *table);
bool agent_policy_table_add(struct agent_policy_table *table,
                            const struct agent_policy_rule *rule);
void agent_policy_select(const struct agent_policy_table *table,
                         const struct route_query *query,
                         struct agent_policy_decision *decision);
enum agent_policy_rejection agent_policy_route_rejection(
    const struct agent_policy_decision *decision,
    const struct agent_route *route);
uint64_t agent_policy_route_score(
    const struct agent_policy_decision *decision,
    const struct agent_route *route);
const char *agent_policy_action_name(enum agent_policy_action action);
void agent_policy_get_admission(
    const struct agent_policy_decision *decision,
    struct agent_policy_admission *admission);

#endif

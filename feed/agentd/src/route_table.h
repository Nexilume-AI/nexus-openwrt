#ifndef NEXUS_AGENT_ROUTE_TABLE_H
#define NEXUS_AGENT_ROUTE_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_ROUTE_ID_LEN 33
#define AGENT_INTENT_LEN 128
#define AGENT_URI_LEN 256
#define AGENT_TENANT_LEN 64
#define AGENT_REGION_LEN 32
#define AGENT_ROUTE_PEER_ID_LEN 65
#define AGENT_ROUTE_ROUTER_ID_LEN 65
#define AGENT_ROUTE_MAX_PATH 8
#define AGENT_SOURCE_AGENT_LEN 256

struct agent_policy_table;
struct agent_policy_admission;

enum route_table_result {
    ROUTE_TABLE_OK = 0,
    ROUTE_TABLE_INVALID = -1,
    ROUTE_TABLE_FULL = -2,
    ROUTE_TABLE_NOT_FOUND = -3,
    ROUTE_TABLE_NO_MEMORY = -4
};

enum agent_route_source {
    AGENT_ROUTE_SOURCE_LOCAL = 0,
    AGENT_ROUTE_SOURCE_STATIC = 1,
    AGENT_ROUTE_SOURCE_PEER = 2,
    AGENT_ROUTE_SOURCE_DISCOVERY = 3
};

struct agent_route {
    char route_id[AGENT_ROUTE_ID_LEN];
    char intent[AGENT_INTENT_LEN];
    uint32_t version;
    char origin[AGENT_URI_LEN];
    char endpoint[AGENT_URI_LEN];
    char tenant[AGENT_TENANT_LEN];
    char region[AGENT_REGION_LEN];
    uint64_t cost_microunits;
    uint32_t latency_ms;
    uint8_t trust_level;
    uint16_t load_permille;
    uint8_t hop_count;
    bool healthy;
    bool reported_healthy;
    uint8_t health_failure_streak;
    uint8_t health_recovery_streak;
    uint64_t lease_expires_ms;
    uint64_t sequence;
    enum agent_route_source source;
    char learned_from_peer[AGENT_ROUTE_PEER_ID_LEN];
    uint64_t snapshot_id;
    uint8_t path_length;
    char path[AGENT_ROUTE_MAX_PATH][AGENT_ROUTE_ROUTER_ID_LEN];
    struct agent_route *previous;
    struct agent_route *next;
    struct agent_route *id_hash_next;
    struct agent_route *intent_hash_next;
};

struct route_query {
    char intent[AGENT_INTENT_LEN];
    uint32_t version;
    char tenant[AGENT_TENANT_LEN];
    char region[AGENT_REGION_LEN];
    char source_agent[AGENT_SOURCE_AGENT_LEN];
    char target_agent[AGENT_URI_LEN];
    uint64_t max_cost_microunits;
    uint32_t max_latency_ms;
    uint8_t min_trust_level;
};

struct route_selection {
    const struct agent_route *route;
    uint64_t score;
};

struct route_diagnostics {
    size_t total;
    size_t eligible;
    size_t intent_mismatch;
    size_t target_agent_mismatch;
    size_t version_mismatch;
    size_t tenant_denied;
    size_t region_mismatch;
    size_t unhealthy;
    size_t expired;
    size_t cost_exceeded;
    size_t latency_exceeded;
    size_t trust_too_low;
    size_t policy_denied;
    size_t policy_region_mismatch;
    size_t policy_cost_exceeded;
    size_t policy_latency_exceeded;
    size_t policy_trust_too_low;
    size_t policy_load_exceeded;
    size_t policy_hops_exceeded;
    size_t policy_source_denied;
    size_t policy_peer_denied;
    bool policy_matched;
    bool policy_allowed;
    char policy_id[65];
};

struct route_renewal {
    uint64_t lease_expires_ms;
    bool update_latency;
    uint32_t latency_ms;
    bool update_load;
    uint16_t load_permille;
    bool update_health;
    bool healthy;
};

struct route_table {
    struct agent_route *head;
    size_t count;
    size_t max_routes;
    uint64_t generation;
    const struct agent_policy_table *policy;
    struct agent_route **id_buckets;
    struct agent_route **intent_buckets;
    size_t bucket_count;
};

void route_table_init(struct route_table *table, size_t max_routes);
void route_table_destroy(struct route_table *table);
void route_table_set_policy(
    struct route_table *table,
    const struct agent_policy_table *policy
);
void route_table_policy_admission(
    const struct route_table *table,
    const struct route_query *query,
    struct agent_policy_admission *admission
);

enum route_table_result route_table_upsert(
    struct route_table *table,
    const struct agent_route *route
);

enum route_table_result route_table_remove(
    struct route_table *table,
    const char *route_id
);

enum route_table_result route_table_renew(
    struct route_table *table,
    const char *route_id,
    const struct route_renewal *renewal
);

size_t route_table_prune_expired(struct route_table *table, uint64_t now_ms);

enum route_table_result route_table_clone_excluding_source(
    struct route_table *destination,
    const struct route_table *source,
    enum agent_route_source excluded_source
);

void route_table_swap(struct route_table *left, struct route_table *right);

bool route_table_lookup(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selection
);

bool route_table_lookup_id(
    const struct route_table *table,
    const struct route_query *query,
    const char *route_id,
    uint64_t now_ms,
    struct route_selection *selection
);

bool route_table_lookup_ex(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selection,
    struct route_diagnostics *diagnostics
);

bool route_table_lookup_local(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selection
);

/* Selects an eligible reflected peer route whose path originates at
 * target_router_id. excluded_peer_id prevents an Invoke from being sent
 * straight back to the peer that delivered it. */
bool route_table_lookup_peer_target(
    const struct route_table *table,
    const struct route_query *query,
    const char *target_router_id,
    const char *excluded_peer_id,
    uint64_t now_ms,
    struct route_selection *selection
);

size_t route_table_lookup_candidates_ex(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selections,
    size_t capacity,
    struct route_diagnostics *diagnostics
);

const struct agent_route *route_table_first(const struct route_table *table);
const struct agent_route *route_table_find(
    const struct route_table *table,
    const char *route_id
);
uint64_t agent_route_score(const struct agent_route *route);
const char *agent_route_source_name(enum agent_route_source source);
bool route_table_index_enabled(const struct route_table *table);
size_t route_table_index_buckets(const struct route_table *table);
size_t route_table_memory_bytes(const struct route_table *table);

#endif

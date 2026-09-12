#include "route_table.h"
#include "agent_policy.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ROUTE_TABLE_MIN_BUCKETS 64U
#define ROUTE_TABLE_MAX_BUCKETS 65536U
#define ROUTE_HASH_OFFSET UINT64_C(14695981039346656037)
#define ROUTE_HASH_PRIME UINT64_C(1099511628211)

static uint64_t route_hash_string(uint64_t hash, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    while (*cursor != '\0') {
        hash ^= (uint64_t)*cursor++;
        hash *= ROUTE_HASH_PRIME;
    }
    return hash;
}

static size_t route_bucket_count(size_t max_routes)
{
    size_t target = max_routes;
    size_t buckets = ROUTE_TABLE_MIN_BUCKETS;

    if (target > ROUTE_TABLE_MAX_BUCKETS) target = ROUTE_TABLE_MAX_BUCKETS;
    while (buckets < target && buckets < ROUTE_TABLE_MAX_BUCKETS) {
        buckets <<= 1U;
    }
    return buckets;
}

static size_t route_id_bucket(
    const struct route_table *table,
    const char *route_id
)
{
    uint64_t hash = route_hash_string(ROUTE_HASH_OFFSET, route_id);
    return (size_t)(hash & (uint64_t)(table->bucket_count - 1U));
}

static size_t route_intent_bucket(
    const struct route_table *table,
    const char *intent,
    uint32_t version
)
{
    uint64_t hash = route_hash_string(ROUTE_HASH_OFFSET, intent);
    unsigned int shift;

    for (shift = 0U; shift < 32U; shift += 8U) {
        hash ^= (uint64_t)((version >> shift) & 0xffU);
        hash *= ROUTE_HASH_PRIME;
    }
    return (size_t)(hash & (uint64_t)(table->bucket_count - 1U));
}

bool route_table_index_enabled(const struct route_table *table)
{
    return table != NULL && table->bucket_count > 0U &&
           table->id_buckets != NULL && table->intent_buckets != NULL;
}

size_t route_table_index_buckets(const struct route_table *table)
{
    return route_table_index_enabled(table) ? table->bucket_count : 0U;
}

size_t route_table_memory_bytes(const struct route_table *table)
{
    size_t bytes;

    if (table == NULL) return 0U;
    bytes = sizeof(*table) + (table->count * sizeof(struct agent_route));
    if (route_table_index_enabled(table)) {
        bytes += 2U * table->bucket_count * sizeof(struct agent_route *);
    }
    return bytes;
}

static void route_index_insert(
    struct route_table *table,
    struct agent_route *route
)
{
    size_t bucket;

    if (!route_table_index_enabled(table) || route == NULL) return;
    bucket = route_id_bucket(table, route->route_id);
    route->id_hash_next = table->id_buckets[bucket];
    table->id_buckets[bucket] = route;
    bucket = route_intent_bucket(table, route->intent, route->version);
    route->intent_hash_next = table->intent_buckets[bucket];
    table->intent_buckets[bucket] = route;
}

static void route_index_remove(
    struct route_table *table,
    struct agent_route *route
)
{
    struct agent_route **cursor;
    size_t bucket;

    if (!route_table_index_enabled(table) || route == NULL) return;
    bucket = route_id_bucket(table, route->route_id);
    cursor = &table->id_buckets[bucket];
    while (*cursor != NULL && *cursor != route) {
        cursor = &(*cursor)->id_hash_next;
    }
    if (*cursor == route) *cursor = route->id_hash_next;

    bucket = route_intent_bucket(table, route->intent, route->version);
    cursor = &table->intent_buckets[bucket];
    while (*cursor != NULL && *cursor != route) {
        cursor = &(*cursor)->intent_hash_next;
    }
    if (*cursor == route) *cursor = route->intent_hash_next;
    route->id_hash_next = NULL;
    route->intent_hash_next = NULL;
}

static void route_list_remove(
    struct route_table *table,
    struct agent_route *route
)
{
    if (table == NULL || route == NULL) return;
    if (route->previous == NULL) table->head = route->next;
    else route->previous->next = route->next;
    if (route->next != NULL) route->next->previous = route->previous;
    route->previous = NULL;
    route->next = NULL;
}

static bool is_nonempty_terminated(const char *value, size_t capacity)
{
    return value != NULL &&
           value[0] != '\0' &&
           memchr(value, '\0', capacity) != NULL;
}

static bool route_is_valid(const struct agent_route *route)
{
    size_t i;

    if (route == NULL) {
        return false;
    }

    if (!is_nonempty_terminated(route->route_id, sizeof(route->route_id)) ||
        !is_nonempty_terminated(route->intent, sizeof(route->intent)) ||
        !is_nonempty_terminated(route->origin, sizeof(route->origin)) ||
        !is_nonempty_terminated(route->endpoint, sizeof(route->endpoint)) ||
        !is_nonempty_terminated(route->tenant, sizeof(route->tenant)) ||
        !is_nonempty_terminated(route->region, sizeof(route->region))) {
        return false;
    }

    if ((route->source == AGENT_ROUTE_SOURCE_PEER &&
         (!is_nonempty_terminated(route->learned_from_peer,
                                  sizeof(route->learned_from_peer)) ||
          route->path_length == 0U ||
          route->path_length > AGENT_ROUTE_MAX_PATH)) ||
        (route->source != AGENT_ROUTE_SOURCE_PEER &&
         (route->learned_from_peer[0] != '\0' ||
          route->path_length != 0U))) {
        return false;
    }
    for (i = 0U; i < route->path_length; i++) {
        if (!is_nonempty_terminated(route->path[i],
                                    sizeof(route->path[i]))) {
            return false;
        }
    }

    return route->version > 0U &&
           route->trust_level <= 100U &&
           route->load_permille <= 1000U &&
           route->source >= AGENT_ROUTE_SOURCE_LOCAL &&
           route->source <= AGENT_ROUTE_SOURCE_DISCOVERY &&
           route->lease_expires_ms > 0U;
}

static bool query_is_valid(const struct route_query *query)
{
    if (query == NULL) {
        return false;
    }

    return is_nonempty_terminated(query->intent, sizeof(query->intent)) &&
           is_nonempty_terminated(query->tenant, sizeof(query->tenant)) &&
           memchr(query->source_agent, '\0', sizeof(query->source_agent)) != NULL &&
           memchr(query->target_agent, '\0', sizeof(query->target_agent)) != NULL &&
           query->version > 0U &&
           query->min_trust_level <= 100U;
}

static struct agent_route *find_route(
    struct route_table *table,
    const char *route_id
)
{
    struct agent_route *route;

    if (table == NULL || route_id == NULL) {
        return NULL;
    }

    if (route_table_index_enabled(table)) {
        route = table->id_buckets[route_id_bucket(table, route_id)];
        for (; route != NULL; route = route->id_hash_next) {
            if (strcmp(route->route_id, route_id) == 0) return route;
        }
        return NULL;
    }

    for (route = table->head; route != NULL; route = route->next) {
        if (strcmp(route->route_id, route_id) == 0) {
            return route;
        }
    }

    return NULL;
}

static void apply_health_sample(
    struct agent_route *route,
    bool reported_healthy,
    uint8_t failure_threshold,
    uint8_t recovery_threshold
)
{
    if (route == NULL) return;
    route->reported_healthy = reported_healthy;
    if (failure_threshold == 0U) failure_threshold = 1U;
    if (recovery_threshold == 0U) recovery_threshold = 1U;
    if (reported_healthy) {
        route->health_failure_streak = 0U;
        if (route->healthy) {
            route->health_recovery_streak = 0U;
            return;
        }
        if (route->health_recovery_streak < UINT8_MAX)
            route->health_recovery_streak++;
        if (route->health_recovery_streak >= recovery_threshold) {
            route->healthy = true;
            route->health_recovery_streak = 0U;
        }
        return;
    }
    route->health_recovery_streak = 0U;
    if (!route->healthy) {
        route->health_failure_streak = 0U;
        return;
    }
    if (route->health_failure_streak < UINT8_MAX)
        route->health_failure_streak++;
    if (route->health_failure_streak >= failure_threshold) {
        route->healthy = false;
        route->health_failure_streak = 0U;
    }
}

const struct agent_route *route_table_find(
    const struct route_table *table,
    const char *route_id
)
{
    const struct agent_route *route;

    if (table == NULL || route_id == NULL) {
        return NULL;
    }

    if (route_table_index_enabled(table)) {
        route = table->id_buckets[route_id_bucket(table, route_id)];
        for (; route != NULL; route = route->id_hash_next) {
            if (strcmp(route->route_id, route_id) == 0) return route;
        }
        return NULL;
    }

    for (route = table->head; route != NULL; route = route->next) {
        if (strcmp(route->route_id, route_id) == 0) {
            return route;
        }
    }

    return NULL;
}

void route_table_init(struct route_table *table, size_t max_routes)
{
    if (table == NULL) {
        return;
    }

    table->head = NULL;
    table->count = 0U;
    table->max_routes = max_routes;
    table->generation = 1U;
    table->policy = NULL;
    table->bucket_count = route_bucket_count(max_routes);
    table->id_buckets = calloc(table->bucket_count,
                               sizeof(*table->id_buckets));
    table->intent_buckets = calloc(table->bucket_count,
                                   sizeof(*table->intent_buckets));
    if (table->id_buckets == NULL || table->intent_buckets == NULL) {
        free(table->id_buckets);
        free(table->intent_buckets);
        table->id_buckets = NULL;
        table->intent_buckets = NULL;
        table->bucket_count = 0U;
    }
}

void route_table_set_policy(
    struct route_table *table,
    const struct agent_policy_table *policy
)
{
    if (table == NULL) return;
    table->policy = policy;
    table->generation++;
}

void route_table_policy_admission(
    const struct route_table *table,
    const struct route_query *query,
    struct agent_policy_admission *admission
)
{
    struct agent_policy_decision decision;
    agent_policy_select(table == NULL ? NULL : table->policy,
                        query, &decision);
    agent_policy_get_admission(&decision, admission);
}

void route_table_destroy(struct route_table *table)
{
    struct agent_route *route;
    struct agent_route *next;

    if (table == NULL) {
        return;
    }

    route = table->head;
    while (route != NULL) {
        next = route->next;
        free(route);
        route = next;
    }

    table->head = NULL;
    table->count = 0U;
    table->generation = 0U;
    table->policy = NULL;
    free(table->id_buckets);
    free(table->intent_buckets);
    table->id_buckets = NULL;
    table->intent_buckets = NULL;
    table->bucket_count = 0U;
}

enum route_table_result route_table_upsert(
    struct route_table *table,
    const struct agent_route *route
)
{
    struct agent_route *existing;
    struct agent_route *copy;
    struct agent_route *next;

    if (table == NULL || !route_is_valid(route)) {
        return ROUTE_TABLE_INVALID;
    }

    existing = find_route(table, route->route_id);
    if (existing != NULL) {
        bool previous_healthy = existing->healthy;
        uint8_t failure_streak = existing->health_failure_streak;
        uint8_t recovery_streak = existing->health_recovery_streak;
        uint8_t failure_threshold = table->policy == NULL ? 1U :
            table->policy->health_failure_threshold;
        uint8_t recovery_threshold = table->policy == NULL ? 1U :
            table->policy->health_recovery_threshold;
        route_index_remove(table, existing);
        next = existing->next;
        copy = existing->previous;
        *existing = *route;
        existing->previous = copy;
        existing->next = next;
        existing->healthy = previous_healthy;
        existing->health_failure_streak = failure_streak;
        existing->health_recovery_streak = recovery_streak;
        apply_health_sample(existing, route->healthy,
                            failure_threshold, recovery_threshold);
        route_index_insert(table, existing);
        table->generation++;
        return ROUTE_TABLE_OK;
    }

    if (table->count >= table->max_routes) {
        return ROUTE_TABLE_FULL;
    }

    copy = calloc(1U, sizeof(*copy));
    if (copy == NULL) {
        return ROUTE_TABLE_NO_MEMORY;
    }

    *copy = *route;
    copy->reported_healthy = route->healthy;
    copy->health_failure_streak = 0U;
    copy->health_recovery_streak = 0U;
    copy->previous = NULL;
    copy->next = table->head;
    copy->id_hash_next = NULL;
    copy->intent_hash_next = NULL;
    if (table->head != NULL) table->head->previous = copy;
    table->head = copy;
    route_index_insert(table, copy);
    table->count++;
    table->generation++;
    return ROUTE_TABLE_OK;
}

enum route_table_result route_table_remove(
    struct route_table *table,
    const char *route_id
)
{
    struct agent_route *removed;

    if (table == NULL || route_id == NULL || route_id[0] == '\0') {
        return ROUTE_TABLE_INVALID;
    }

    removed = find_route(table, route_id);
    if (removed == NULL) return ROUTE_TABLE_NOT_FOUND;
    route_index_remove(table, removed);
    route_list_remove(table, removed);
    free(removed);
    table->count--;
    table->generation++;
    return ROUTE_TABLE_OK;
}

enum route_table_result route_table_renew(
    struct route_table *table,
    const char *route_id,
    const struct route_renewal *renewal
)
{
    struct agent_route *route;

    if (table == NULL ||
        route_id == NULL ||
        route_id[0] == '\0' ||
        renewal == NULL ||
        renewal->lease_expires_ms == 0U ||
        (renewal->update_load && renewal->load_permille > 1000U)) {
        return ROUTE_TABLE_INVALID;
    }

    route = find_route(table, route_id);
    if (route == NULL) {
        return ROUTE_TABLE_NOT_FOUND;
    }

    route->lease_expires_ms = renewal->lease_expires_ms;
    if (renewal->update_latency) {
        route->latency_ms = renewal->latency_ms;
    }
    if (renewal->update_load) {
        route->load_permille = renewal->load_permille;
    }
    if (renewal->update_health) {
        apply_health_sample(
            route, renewal->healthy,
            table->policy == NULL ? 1U :
                table->policy->health_failure_threshold,
            table->policy == NULL ? 1U :
                table->policy->health_recovery_threshold);
    }
    route->sequence++;
    table->generation++;
    return ROUTE_TABLE_OK;
}

size_t route_table_prune_expired(struct route_table *table, uint64_t now_ms)
{
    struct agent_route *route;
    struct agent_route *next;
    struct agent_route *removed;
    size_t pruned = 0U;

    if (table == NULL) {
        return 0U;
    }

    route = table->head;
    while (route != NULL) {
        next = route->next;
        if (route->lease_expires_ms <= now_ms) {
            removed = route;
            route_index_remove(table, removed);
            route_list_remove(table, removed);
            free(removed);
            table->count--;
            pruned++;
        }
        route = next;
    }

    if (pruned > 0U) {
        table->generation++;
    }

    return pruned;
}

enum route_table_result route_table_clone_excluding_source(
    struct route_table *destination,
    const struct route_table *source,
    enum agent_route_source excluded_source
)
{
    const struct agent_route *route;
    enum route_table_result result;

    if (destination == NULL ||
        source == NULL ||
        excluded_source > AGENT_ROUTE_SOURCE_DISCOVERY) {
        return ROUTE_TABLE_INVALID;
    }

    route_table_init(destination, source->max_routes);
    destination->policy = source->policy;
    for (route = source->head; route != NULL; route = route->next) {
        if (route->source == excluded_source) {
            continue;
        }

        result = route_table_upsert(destination, route);
        if (result != ROUTE_TABLE_OK) {
            route_table_destroy(destination);
            return result;
        }
    }

    destination->generation = source->generation;
    return ROUTE_TABLE_OK;
}

void route_table_swap(struct route_table *left, struct route_table *right)
{
    struct route_table temporary;

    if (left == NULL || right == NULL) {
        return;
    }

    temporary = *left;
    *left = *right;
    *right = temporary;
}

uint64_t agent_route_score(const struct agent_route *route)
{
    uint64_t trust_penalty;

    if (route == NULL) {
        return UINT64_MAX;
    }

    trust_penalty = (uint64_t)(100U - route->trust_level);
    return ((uint64_t)route->latency_ms * 1000U) +
           (route->cost_microunits / 100U) +
           ((uint64_t)route->load_permille * 100U) +
           (trust_penalty * 5000U) +
           ((uint64_t)route->hop_count * 10000U);
}

static bool value_matches(const char *route_value, const char *query_value)
{
    return strcmp(route_value, "*") == 0 ||
           strcmp(route_value, query_value) == 0;
}

enum route_rejection {
    ROUTE_ELIGIBLE = 0,
    ROUTE_REJECT_INTENT,
    ROUTE_REJECT_TARGET_AGENT,
    ROUTE_REJECT_VERSION,
    ROUTE_REJECT_TENANT,
    ROUTE_REJECT_REGION,
    ROUTE_REJECT_UNHEALTHY,
    ROUTE_REJECT_EXPIRED,
    ROUTE_REJECT_COST,
    ROUTE_REJECT_LATENCY,
    ROUTE_REJECT_TRUST
};

static enum route_rejection route_rejection_reason(
    const struct agent_route *route,
    const struct route_query *query,
    uint64_t now_ms
)
{
    if (strcmp(route->intent, query->intent) != 0) {
        return ROUTE_REJECT_INTENT;
    }
    if (query->target_agent[0] != '\0' &&
        strcmp(route->origin, query->target_agent) != 0) {
        return ROUTE_REJECT_TARGET_AGENT;
    }
    if (route->version != query->version) {
        return ROUTE_REJECT_VERSION;
    }
    if (!value_matches(route->tenant, query->tenant)) {
        return ROUTE_REJECT_TENANT;
    }
    if (query->region[0] != '\0' &&
        !value_matches(route->region, query->region)) {
        return ROUTE_REJECT_REGION;
    }
    if (!route->healthy) {
        return ROUTE_REJECT_UNHEALTHY;
    }
    if (route->lease_expires_ms <= now_ms) {
        return ROUTE_REJECT_EXPIRED;
    }
    if (query->max_cost_microunits > 0U &&
        route->cost_microunits > query->max_cost_microunits) {
        return ROUTE_REJECT_COST;
    }
    if (query->max_latency_ms > 0U &&
        route->latency_ms > query->max_latency_ms) {
        return ROUTE_REJECT_LATENCY;
    }
    if (route->trust_level < query->min_trust_level) {
        return ROUTE_REJECT_TRUST;
    }

    return ROUTE_ELIGIBLE;
}

static void record_rejection(
    struct route_diagnostics *diagnostics,
    enum route_rejection rejection
)
{
    if (diagnostics == NULL) {
        return;
    }

    diagnostics->total++;
    switch (rejection) {
    case ROUTE_ELIGIBLE:
        diagnostics->eligible++;
        break;
    case ROUTE_REJECT_INTENT:
        diagnostics->intent_mismatch++;
        break;
    case ROUTE_REJECT_TARGET_AGENT:
        diagnostics->target_agent_mismatch++;
        break;
    case ROUTE_REJECT_VERSION:
        diagnostics->version_mismatch++;
        break;
    case ROUTE_REJECT_TENANT:
        diagnostics->tenant_denied++;
        break;
    case ROUTE_REJECT_REGION:
        diagnostics->region_mismatch++;
        break;
    case ROUTE_REJECT_UNHEALTHY:
        diagnostics->unhealthy++;
        break;
    case ROUTE_REJECT_EXPIRED:
        diagnostics->expired++;
        break;
    case ROUTE_REJECT_COST:
        diagnostics->cost_exceeded++;
        break;
    case ROUTE_REJECT_LATENCY:
        diagnostics->latency_exceeded++;
        break;
    case ROUTE_REJECT_TRUST:
        diagnostics->trust_too_low++;
        break;
    }
}

static void record_policy_rejection(
    struct route_diagnostics *diagnostics,
    enum agent_policy_rejection rejection
)
{
    if (diagnostics == NULL || rejection == AGENT_POLICY_ROUTE_ALLOWED) return;
    switch (rejection) {
    case AGENT_POLICY_REJECT_ACTION:
        diagnostics->policy_denied++;
        break;
    case AGENT_POLICY_REJECT_REGION:
        diagnostics->policy_region_mismatch++;
        break;
    case AGENT_POLICY_REJECT_COST:
        diagnostics->policy_cost_exceeded++;
        break;
    case AGENT_POLICY_REJECT_LATENCY:
        diagnostics->policy_latency_exceeded++;
        break;
    case AGENT_POLICY_REJECT_TRUST:
        diagnostics->policy_trust_too_low++;
        break;
    case AGENT_POLICY_REJECT_LOAD:
        diagnostics->policy_load_exceeded++;
        break;
    case AGENT_POLICY_REJECT_HOPS:
        diagnostics->policy_hops_exceeded++;
        break;
    case AGENT_POLICY_REJECT_SOURCE:
        diagnostics->policy_source_denied++;
        break;
    case AGENT_POLICY_REJECT_PEER:
        diagnostics->policy_peer_denied++;
        break;
    case AGENT_POLICY_ROUTE_ALLOWED:
        break;
    }
}

bool route_table_lookup(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selection
)
{
    return route_table_lookup_ex(table, query, now_ms, selection, NULL);
}

bool route_table_lookup_ex(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selection,
    struct route_diagnostics *diagnostics
)
{
    return route_table_lookup_candidates_ex(
        table, query, now_ms, selection, 1U, diagnostics) == 1U;
}

bool route_table_lookup_id(
    const struct route_table *table,
    const struct route_query *query,
    const char *route_id,
    uint64_t now_ms,
    struct route_selection *selection
)
{
    const struct agent_route *route;
    struct agent_policy_decision decision;

    if (table == NULL || !query_is_valid(query) || route_id == NULL ||
        route_id[0] == '\0' || selection == NULL) return false;
    memset(selection, 0, sizeof(*selection));
    route = route_table_find(table, route_id);
    if (route == NULL ||
        route_rejection_reason(route, query, now_ms) != ROUTE_ELIGIBLE) {
        return false;
    }
    agent_policy_select(table->policy, query, &decision);
    if (agent_policy_route_rejection(&decision, route) !=
        AGENT_POLICY_ROUTE_ALLOWED) return false;
    selection->route = route;
    selection->score = agent_policy_route_score(&decision, route);
    return true;
}

static bool selection_precedes(
    uint64_t score,
    const struct agent_route *route,
    const struct route_selection *selection
)
{
    return score < selection->score ||
           (score == selection->score &&
            strcmp(route->route_id, selection->route->route_id) < 0);
}

bool route_table_lookup_local(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selection
)
{
    const struct agent_route *route;
    uint64_t score;
    bool found = false;
    bool indexed;
    struct agent_policy_decision decision;

    if (table == NULL || !query_is_valid(query) || selection == NULL) {
        return false;
    }
    agent_policy_select(table->policy, query, &decision);
    memset(selection, 0, sizeof(*selection));
    indexed = route_table_index_enabled(table);
    route = indexed ?
        table->intent_buckets[route_intent_bucket(
            table, query->intent, query->version)] : table->head;
    for (; route != NULL;
         route = indexed ? route->intent_hash_next : route->next) {
        if ((route->source != AGENT_ROUTE_SOURCE_LOCAL &&
             route->source != AGENT_ROUTE_SOURCE_STATIC) ||
            route_rejection_reason(route, query, now_ms) != ROUTE_ELIGIBLE ||
            agent_policy_route_rejection(&decision, route) !=
                AGENT_POLICY_ROUTE_ALLOWED) {
            continue;
        }
        score = agent_policy_route_score(&decision, route);
        if (!found || selection_precedes(score, route, selection)) {
            selection->route = route;
            selection->score = score;
            found = true;
        }
    }
    return found;
}

bool route_table_lookup_peer_target(
    const struct route_table *table,
    const struct route_query *query,
    const char *target_router_id,
    const char *excluded_peer_id,
    uint64_t now_ms,
    struct route_selection *selection
)
{
    const struct agent_route *route;
    uint64_t score;
    bool found = false;
    bool indexed;
    struct agent_policy_decision decision;

    if (table == NULL || !query_is_valid(query) ||
        target_router_id == NULL || target_router_id[0] == '\0' ||
        selection == NULL) {
        return false;
    }
    agent_policy_select(table->policy, query, &decision);
    memset(selection, 0, sizeof(*selection));
    indexed = route_table_index_enabled(table);
    route = indexed ?
        table->intent_buckets[route_intent_bucket(
            table, query->intent, query->version)] : table->head;
    for (; route != NULL;
         route = indexed ? route->intent_hash_next : route->next) {
        if (route->source != AGENT_ROUTE_SOURCE_PEER ||
            route->path_length == 0U ||
            strcmp(route->path[0], target_router_id) != 0 ||
            route->learned_from_peer[0] == '\0' ||
            (excluded_peer_id != NULL && excluded_peer_id[0] != '\0' &&
             strcmp(route->learned_from_peer, excluded_peer_id) == 0) ||
            route_rejection_reason(route, query, now_ms) != ROUTE_ELIGIBLE ||
            agent_policy_route_rejection(&decision, route) !=
                AGENT_POLICY_ROUTE_ALLOWED) {
            continue;
        }
        score = agent_policy_route_score(&decision, route);
        if (!found || selection_precedes(score, route, selection)) {
            selection->route = route;
            selection->score = score;
            found = true;
        }
    }
    return found;
}

size_t route_table_lookup_candidates_ex(
    const struct route_table *table,
    const struct route_query *query,
    uint64_t now_ms,
    struct route_selection *selections,
    size_t capacity,
    struct route_diagnostics *diagnostics
)
{
    const struct agent_route *route;
    enum route_rejection rejection;
    uint64_t score;
    size_t count = 0U;
    size_t insert_at;
    size_t move;
    bool indexed;
    struct agent_policy_decision decision;
    enum agent_policy_rejection policy_rejection;

    if (table == NULL || !query_is_valid(query) || selections == NULL ||
        capacity == 0U) {
        return 0U;
    }

    if (diagnostics != NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
    }
    agent_policy_select(table->policy, query, &decision);
    if (diagnostics != NULL) {
        diagnostics->policy_matched = decision.matched;
        diagnostics->policy_allowed = decision.allow;
        if (decision.rule != NULL) {
            (void)snprintf(diagnostics->policy_id,
                           sizeof(diagnostics->policy_id), "%s",
                           decision.rule->policy_id);
        }
    }

    /* Explain/diagnostics retains full-table rejection counts. The production
     * lookup path has no diagnostics and can use the exact intent index. */
    indexed = diagnostics == NULL && route_table_index_enabled(table);
    route = indexed ?
        table->intent_buckets[route_intent_bucket(
            table, query->intent, query->version)] : table->head;
    for (; route != NULL;
         route = indexed ? route->intent_hash_next : route->next) {
        rejection = route_rejection_reason(route, query, now_ms);
        record_rejection(diagnostics, rejection);
        if (rejection != ROUTE_ELIGIBLE) {
            continue;
        }

        policy_rejection = agent_policy_route_rejection(&decision, route);
        record_policy_rejection(diagnostics, policy_rejection);
        if (policy_rejection != AGENT_POLICY_ROUTE_ALLOWED) continue;

        score = agent_policy_route_score(&decision, route);
        insert_at = count;
        while (insert_at > 0U && selection_precedes(
                   score, route, &selections[insert_at - 1U])) {
            insert_at--;
        }
        if (insert_at >= capacity) {
            continue;
        }
        move = count < capacity ? count : capacity - 1U;
        while (move > insert_at) {
            selections[move] = selections[move - 1U];
            move--;
        }
        selections[insert_at].route = route;
        selections[insert_at].score = score;
        if (count < capacity) {
            count++;
        }
    }
    return count;
}

const struct agent_route *route_table_first(const struct route_table *table)
{
    return table == NULL ? NULL : table->head;
}

const char *agent_route_source_name(enum agent_route_source source)
{
    switch (source) {
    case AGENT_ROUTE_SOURCE_LOCAL:
        return "local";
    case AGENT_ROUTE_SOURCE_STATIC:
        return "static";
    case AGENT_ROUTE_SOURCE_PEER:
        return "peer";
    case AGENT_ROUTE_SOURCE_DISCOVERY:
        return "discovery";
    default:
        return "unknown";
    }
}

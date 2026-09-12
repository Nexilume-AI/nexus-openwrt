#include "agent_discovery_promotion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) (*value)++;
}

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') return false;
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static const struct agent_cross_candidate *cross_find(
    const struct agent_cross_table *table,
    const char *router_id
)
{
    const struct agent_cross_candidate *candidate;

    if (table == NULL || router_id == NULL) return NULL;
    for (candidate = agent_cross_table_first(table); candidate != NULL;
         candidate = candidate->next) {
        if (strcmp(candidate->router_id, router_id) == 0) return candidate;
    }
    return NULL;
}

static bool peer_equal(
    const struct agent_peer *left,
    const struct agent_peer *right
)
{
    return left != NULL && right != NULL &&
           strcmp(left->peer_id, right->peer_id) == 0 &&
           strcmp(left->router_id, right->router_id) == 0 &&
           strcmp(left->domain_id, right->domain_id) == 0 &&
           strcmp(left->endpoint, right->endpoint) == 0 &&
           strcmp(left->connect_ipv4, right->connect_ipv4) == 0 &&
           left->role == right->role &&
           left->graceful_restart_seconds ==
               right->graceful_restart_seconds;
}

static bool build_lan_peer(
    const struct agent_discovery_candidate *candidate,
    const char *peer_id,
    uint32_t graceful_restart_seconds,
    struct agent_peer *peer
)
{
    const struct agent_discovery_observation *observation;
    int written;

    if (candidate == NULL || peer == NULL) return false;
    observation = &candidate->observation;
    memset(peer, 0, sizeof(*peer));
    if (!copy_text(peer->peer_id, sizeof(peer->peer_id), peer_id) ||
        !copy_text(peer->router_id, sizeof(peer->router_id),
                   observation->router_id) ||
        !copy_text(peer->domain_id, sizeof(peer->domain_id),
                   observation->domain_id) ||
        !copy_text(peer->connect_ipv4, sizeof(peer->connect_ipv4),
                   observation->ipv4)) return false;
    written = snprintf(peer->endpoint, sizeof(peer->endpoint),
                       "https://%s.%s:%u%s", observation->router_id,
                       observation->domain_id,
                       (unsigned int)observation->port,
                       observation->registration_path);
    if (written < 0 || (size_t)written >= sizeof(peer->endpoint)) return false;
    peer->role = AGENT_PEER_ROLE_DIRECT;
    peer->state = AGENT_PEER_STATE_CONFIGURED;
    peer->graceful_restart_seconds = graceful_restart_seconds;
    return agent_peer_valid(peer);
}

static bool build_svcb_peer(
    const struct agent_cross_candidate *candidate,
    const char *peer_id,
    uint32_t graceful_restart_seconds,
    struct agent_peer *peer
)
{
    int written;

    if (candidate == NULL || peer == NULL ||
        candidate->ipv4_hint[0] == '\0') return false;
    memset(peer, 0, sizeof(*peer));
    if (!copy_text(peer->peer_id, sizeof(peer->peer_id), peer_id) ||
        !copy_text(peer->router_id, sizeof(peer->router_id),
                   candidate->router_id) ||
        !copy_text(peer->domain_id, sizeof(peer->domain_id),
                   candidate->domain_id) ||
        !copy_text(peer->connect_ipv4, sizeof(peer->connect_ipv4),
                   candidate->ipv4_hint)) return false;
    written = snprintf(peer->endpoint, sizeof(peer->endpoint),
                       "https://%s:%u/arpx/v1", candidate->target,
                       (unsigned int)candidate->port);
    if (written < 0 || (size_t)written >= sizeof(peer->endpoint)) return false;
    peer->role = AGENT_PEER_ROLE_DIRECT;
    peer->state = AGENT_PEER_STATE_CONFIGURED;
    peer->graceful_restart_seconds = graceful_restart_seconds;
    return agent_peer_valid(peer);
}

static struct agent_discovery_promotion *find_mutable(
    struct agent_discovery_promotion_manager *manager,
    const char *peer_id
)
{
    struct agent_discovery_promotion *entry;

    if (manager == NULL || peer_id == NULL) return NULL;
    for (entry = manager->head; entry != NULL; entry = entry->next) {
        if (strcmp(entry->peer.peer_id, peer_id) == 0) return entry;
    }
    return NULL;
}

static enum agent_promotion_result promote(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    enum agent_promotion_source source,
    const char *router_id,
    const struct agent_peer *peer,
    uint64_t candidate_generation,
    uint64_t expires_at_ms,
    uint64_t now_ms,
    bool automatic
)
{
    struct agent_discovery_promotion *entry;
    struct agent_discovery_promotion *scan;
    enum peer_table_result peer_result;

    if (manager == NULL || peers == NULL || router_id == NULL ||
        peer == NULL || expires_at_ms <= now_ms) return AGENT_PROMOTION_INVALID;
    entry = find_mutable(manager, peer->peer_id);
    if (entry != NULL) {
        if (entry->source != source ||
            strcmp(entry->router_id, router_id) != 0 ||
            !peer_equal(&entry->peer, peer)) {
            increment(&manager->conflicts);
            return AGENT_PROMOTION_CONFLICT;
        }
        entry->candidate_generation = candidate_generation;
        entry->expires_at_ms = expires_at_ms;
        if (!automatic) entry->automatic = false;
        increment(&manager->renewed);
        increment(&manager->generation);
        return AGENT_PROMOTION_OK;
    }
    for (scan = manager->head; scan != NULL; scan = scan->next) {
        if (scan->source == source &&
            strcmp(scan->router_id, router_id) == 0) {
            increment(&manager->conflicts);
            return AGENT_PROMOTION_CONFLICT;
        }
    }
    if (peer_table_find(peers, peer->peer_id) != NULL) {
        increment(&manager->conflicts);
        return AGENT_PROMOTION_CONFLICT;
    }
    if (manager->count >= manager->max_promotions) {
        return AGENT_PROMOTION_FULL;
    }
    entry = calloc(1U, sizeof(*entry));
    if (entry == NULL) return AGENT_PROMOTION_NO_MEMORY;
    entry->source = source;
    entry->peer = *peer;
    entry->peer.next = NULL;
    (void)copy_text(entry->router_id, sizeof(entry->router_id), router_id);
    entry->candidate_generation = candidate_generation;
    entry->approved_at_ms = now_ms;
    entry->expires_at_ms = expires_at_ms;
    entry->automatic = automatic;
    peer_result = peer_table_upsert(peers, peer);
    if (peer_result != PEER_TABLE_OK) {
        free(entry);
        if (peer_result == PEER_TABLE_FULL) return AGENT_PROMOTION_FULL;
        if (peer_result == PEER_TABLE_NO_MEMORY) {
            return AGENT_PROMOTION_NO_MEMORY;
        }
        increment(&manager->conflicts);
        return AGENT_PROMOTION_CONFLICT;
    }
    entry->next = manager->head;
    manager->head = entry;
    manager->count++;
    increment(&manager->promoted);
    increment(&manager->generation);
    return AGENT_PROMOTION_OK;
}

void agent_discovery_promotion_init(
    struct agent_discovery_promotion_manager *manager,
    size_t max_promotions
)
{
    if (manager == NULL) return;
    memset(manager, 0, sizeof(*manager));
    manager->max_promotions = max_promotions;
}

void agent_discovery_promotion_destroy(
    struct agent_discovery_promotion_manager *manager
)
{
    struct agent_discovery_promotion *entry;
    struct agent_discovery_promotion *next;

    if (manager == NULL) return;
    for (entry = manager->head; entry != NULL; entry = next) {
        next = entry->next;
        free(entry);
    }
    memset(manager, 0, sizeof(*manager));
}

enum agent_promotion_result agent_discovery_promote_lan(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_discovery_table *candidates,
    const char *router_id,
    const char *peer_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
)
{
    const struct agent_discovery_candidate *candidate;
    struct agent_peer peer;

    if (manager == NULL || peers == NULL || candidates == NULL ||
        router_id == NULL || peer_id == NULL) return AGENT_PROMOTION_INVALID;
    if (expected_generation != candidates->generation) {
        return AGENT_PROMOTION_STALE_GENERATION;
    }
    candidate = agent_discovery_table_find(candidates, router_id);
    if (candidate == NULL || candidate->expires_at_ms <= now_ms) {
        return AGENT_PROMOTION_NOT_FOUND;
    }
    if (!build_lan_peer(candidate, peer_id, graceful_restart_seconds, &peer)) {
        return AGENT_PROMOTION_INVALID;
    }
    return promote(manager, peers, AGENT_PROMOTION_LAN, router_id, &peer,
                   expected_generation, candidate->expires_at_ms, now_ms,
                   false);
}

enum agent_promotion_result agent_discovery_promote_lan_auto(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_discovery_table *candidates,
    const char *router_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
)
{
    const struct agent_discovery_candidate *candidate;
    struct agent_peer peer;

    if (manager == NULL || peers == NULL || candidates == NULL ||
        router_id == NULL) return AGENT_PROMOTION_INVALID;
    if (expected_generation != candidates->generation) {
        return AGENT_PROMOTION_STALE_GENERATION;
    }
    candidate = agent_discovery_table_find(candidates, router_id);
    if (candidate == NULL || candidate->expires_at_ms <= now_ms) {
        return AGENT_PROMOTION_NOT_FOUND;
    }
    if (!build_lan_peer(candidate, router_id,
                        graceful_restart_seconds, &peer)) {
        return AGENT_PROMOTION_INVALID;
    }
    return promote(manager, peers, AGENT_PROMOTION_LAN, router_id, &peer,
                   expected_generation, candidate->expires_at_ms, now_ms,
                   true);
}

enum agent_promotion_result agent_discovery_promote_svcb(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_cross_table *candidates,
    const char *router_id,
    const char *peer_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
)
{
    const struct agent_cross_candidate *candidate;
    struct agent_peer peer;

    if (manager == NULL || peers == NULL || candidates == NULL ||
        router_id == NULL || peer_id == NULL) return AGENT_PROMOTION_INVALID;
    if (expected_generation != candidates->generation) {
        return AGENT_PROMOTION_STALE_GENERATION;
    }
    candidate = cross_find(candidates, router_id);
    if (candidate == NULL || candidate->expires_at_ms <= now_ms) {
        return AGENT_PROMOTION_NOT_FOUND;
    }
    if (candidate->ipv4_hint[0] == '\0') {
        return AGENT_PROMOTION_ADDRESS_REQUIRED;
    }
    if (!build_svcb_peer(candidate, peer_id, graceful_restart_seconds, &peer)) {
        return AGENT_PROMOTION_INVALID;
    }
    return promote(manager, peers, AGENT_PROMOTION_SVCB, router_id, &peer,
                   expected_generation, candidate->expires_at_ms, now_ms,
                   false);
}

enum agent_promotion_result agent_discovery_promote_svcb_auto(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_cross_table *candidates,
    const char *router_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
)
{
    const struct agent_cross_candidate *candidate;
    struct agent_peer peer;

    if (manager == NULL || peers == NULL || candidates == NULL ||
        router_id == NULL) return AGENT_PROMOTION_INVALID;
    if (expected_generation != candidates->generation) {
        return AGENT_PROMOTION_STALE_GENERATION;
    }
    candidate = cross_find(candidates, router_id);
    if (candidate == NULL || candidate->expires_at_ms <= now_ms) {
        return AGENT_PROMOTION_NOT_FOUND;
    }
    if (candidate->ipv4_hint[0] == '\0') {
        return AGENT_PROMOTION_ADDRESS_REQUIRED;
    }
    if (!build_svcb_peer(candidate, router_id,
                         graceful_restart_seconds, &peer)) {
        return AGENT_PROMOTION_INVALID;
    }
    return promote(manager, peers, AGENT_PROMOTION_SVCB, router_id, &peer,
                   expected_generation, candidate->expires_at_ms, now_ms,
                   true);
}

enum agent_promotion_result agent_discovery_promotion_remove(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const char *peer_id
)
{
    struct agent_discovery_promotion **link;
    struct agent_discovery_promotion *removed;

    if (manager == NULL || peers == NULL || peer_id == NULL) {
        return AGENT_PROMOTION_INVALID;
    }
    for (link = &manager->head; *link != NULL; link = &(*link)->next) {
        if (strcmp((*link)->peer.peer_id, peer_id) != 0) continue;
        removed = *link;
        *link = removed->next;
        (void)peer_table_remove(peers, peer_id);
        free(removed);
        manager->count--;
        increment(&manager->revoked);
        increment(&manager->generation);
        return AGENT_PROMOTION_OK;
    }
    return AGENT_PROMOTION_NOT_FOUND;
}

static bool lan_still_matches(
    const struct agent_discovery_promotion *entry,
    const struct agent_discovery_table *table,
    uint64_t now_ms,
    uint64_t *expires_at_ms
)
{
    const struct agent_discovery_candidate *candidate;
    struct agent_peer peer;

    candidate = agent_discovery_table_find(table, entry->router_id);
    if (candidate == NULL || candidate->expires_at_ms <= now_ms ||
        !build_lan_peer(candidate, entry->peer.peer_id,
                        entry->peer.graceful_restart_seconds, &peer) ||
        !peer_equal(&entry->peer, &peer)) return false;
    *expires_at_ms = candidate->expires_at_ms;
    return true;
}

static bool svcb_still_matches(
    const struct agent_discovery_promotion *entry,
    const struct agent_cross_table *table,
    uint64_t now_ms,
    uint64_t *expires_at_ms
)
{
    const struct agent_cross_candidate *candidate;
    struct agent_peer peer;

    candidate = cross_find(table, entry->router_id);
    if (candidate == NULL || candidate->expires_at_ms <= now_ms ||
        !build_svcb_peer(candidate, entry->peer.peer_id,
                         entry->peer.graceful_restart_seconds, &peer) ||
        !peer_equal(&entry->peer, &peer)) return false;
    *expires_at_ms = candidate->expires_at_ms;
    return true;
}

size_t agent_discovery_promotion_reconcile(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_discovery_table *lan_candidates,
    const struct agent_cross_table *svcb_candidates,
    uint64_t now_ms,
    agent_promotion_removing_cb removing,
    void *context
)
{
    struct agent_discovery_promotion **link;
    struct agent_discovery_promotion *removed;
    uint64_t expires_at_ms;
    bool valid;
    size_t count = 0U;

    if (manager == NULL || peers == NULL || lan_candidates == NULL ||
        svcb_candidates == NULL) return 0U;
    link = &manager->head;
    while (*link != NULL) {
        expires_at_ms = 0U;
        valid = (*link)->source == AGENT_PROMOTION_LAN
            ? lan_still_matches(*link, lan_candidates, now_ms,
                                &expires_at_ms)
            : svcb_still_matches(*link, svcb_candidates, now_ms,
                                 &expires_at_ms);
        if (valid) {
            (*link)->expires_at_ms = expires_at_ms;
            link = &(*link)->next;
            continue;
        }
        removed = *link;
        if (removing != NULL) removing(context, removed->peer.peer_id);
        (void)peer_table_remove(peers, removed->peer.peer_id);
        *link = removed->next;
        free(removed);
        manager->count--;
        count++;
        increment(&manager->expired);
        increment(&manager->generation);
    }
    return count;
}

enum agent_promotion_result agent_discovery_promotion_reapply(
    const struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers
)
{
    const struct agent_discovery_promotion *entry;
    enum peer_table_result result;

    if (manager == NULL || peers == NULL) return AGENT_PROMOTION_INVALID;
    if (manager->count > peers->max_peers - peers->count) {
        return AGENT_PROMOTION_FULL;
    }
    for (entry = manager->head; entry != NULL; entry = entry->next) {
        if (peer_table_find(peers, entry->peer.peer_id) != NULL) {
            return AGENT_PROMOTION_CONFLICT;
        }
    }
    for (entry = manager->head; entry != NULL; entry = entry->next) {
        result = peer_table_upsert(peers, &entry->peer);
        if (result != PEER_TABLE_OK) {
            if (result == PEER_TABLE_FULL) return AGENT_PROMOTION_FULL;
            if (result == PEER_TABLE_NO_MEMORY) {
                return AGENT_PROMOTION_NO_MEMORY;
            }
            return AGENT_PROMOTION_CONFLICT;
        }
    }
    return AGENT_PROMOTION_OK;
}

const struct agent_discovery_promotion *agent_discovery_promotion_first(
    const struct agent_discovery_promotion_manager *manager
)
{
    return manager == NULL ? NULL : manager->head;
}

const struct agent_discovery_promotion *agent_discovery_promotion_find(
    const struct agent_discovery_promotion_manager *manager,
    const char *peer_id
)
{
    const struct agent_discovery_promotion *entry;

    if (manager == NULL || peer_id == NULL) return NULL;
    for (entry = manager->head; entry != NULL; entry = entry->next) {
        if (strcmp(entry->peer.peer_id, peer_id) == 0) return entry;
    }
    return NULL;
}

const char *agent_promotion_source_name(enum agent_promotion_source source)
{
    return source == AGENT_PROMOTION_LAN ? "lan" :
           source == AGENT_PROMOTION_SVCB ? "svcb" : "unknown";
}

const char *agent_promotion_result_name(enum agent_promotion_result result)
{
    switch (result) {
    case AGENT_PROMOTION_OK: return "ok";
    case AGENT_PROMOTION_INVALID: return "invalid request";
    case AGENT_PROMOTION_NOT_FOUND: return "candidate or promotion not found";
    case AGENT_PROMOTION_STALE_GENERATION: return "stale candidate generation";
    case AGENT_PROMOTION_ADDRESS_REQUIRED: return "candidate IPv4 hint required";
    case AGENT_PROMOTION_CONFLICT: return "peer identity or endpoint conflict";
    case AGENT_PROMOTION_FULL: return "promotion or peer table full";
    case AGENT_PROMOTION_NO_MEMORY: return "out of memory";
    default: return "unknown promotion result";
    }
}

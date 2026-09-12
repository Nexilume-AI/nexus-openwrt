#ifndef NEXUS_AGENT_DISCOVERY_PROMOTION_H
#define NEXUS_AGENT_DISCOVERY_PROMOTION_H

#include "agent_cross_discovery.h"
#include "agent_discovery.h"
#include "peer_table.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum agent_promotion_source {
    AGENT_PROMOTION_LAN = 0,
    AGENT_PROMOTION_SVCB = 1
};

enum agent_promotion_result {
    AGENT_PROMOTION_OK = 0,
    AGENT_PROMOTION_INVALID = -1,
    AGENT_PROMOTION_NOT_FOUND = -2,
    AGENT_PROMOTION_STALE_GENERATION = -3,
    AGENT_PROMOTION_ADDRESS_REQUIRED = -4,
    AGENT_PROMOTION_CONFLICT = -5,
    AGENT_PROMOTION_FULL = -6,
    AGENT_PROMOTION_NO_MEMORY = -7
};

struct agent_discovery_promotion {
    enum agent_promotion_source source;
    char router_id[AGENT_PEER_ID_LEN];
    struct agent_peer peer;
    uint64_t candidate_generation;
    uint64_t approved_at_ms;
    uint64_t expires_at_ms;
    bool automatic;
    struct agent_discovery_promotion *next;
};

struct agent_discovery_promotion_manager {
    struct agent_discovery_promotion *head;
    size_t count;
    size_t max_promotions;
    uint64_t generation;
    uint64_t promoted;
    uint64_t renewed;
    uint64_t revoked;
    uint64_t expired;
    uint64_t conflicts;
};

typedef void (*agent_promotion_removing_cb)(
    void *context,
    const char *peer_id
);

void agent_discovery_promotion_init(
    struct agent_discovery_promotion_manager *manager,
    size_t max_promotions
);
void agent_discovery_promotion_destroy(
    struct agent_discovery_promotion_manager *manager
);

enum agent_promotion_result agent_discovery_promote_lan(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_discovery_table *candidates,
    const char *router_id,
    const char *peer_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
);

enum agent_promotion_result agent_discovery_promote_lan_auto(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_discovery_table *candidates,
    const char *router_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
);

enum agent_promotion_result agent_discovery_promote_svcb(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_cross_table *candidates,
    const char *router_id,
    const char *peer_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
);

enum agent_promotion_result agent_discovery_promote_svcb_auto(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_cross_table *candidates,
    const char *router_id,
    uint64_t expected_generation,
    uint32_t graceful_restart_seconds,
    uint64_t now_ms
);

enum agent_promotion_result agent_discovery_promotion_remove(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const char *peer_id
);

size_t agent_discovery_promotion_reconcile(
    struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers,
    const struct agent_discovery_table *lan_candidates,
    const struct agent_cross_table *svcb_candidates,
    uint64_t now_ms,
    agent_promotion_removing_cb removing,
    void *context
);

enum agent_promotion_result agent_discovery_promotion_reapply(
    const struct agent_discovery_promotion_manager *manager,
    struct peer_table *peers
);

const struct agent_discovery_promotion *agent_discovery_promotion_first(
    const struct agent_discovery_promotion_manager *manager
);
const struct agent_discovery_promotion *agent_discovery_promotion_find(
    const struct agent_discovery_promotion_manager *manager,
    const char *peer_id
);
const char *agent_promotion_source_name(enum agent_promotion_source source);
const char *agent_promotion_result_name(enum agent_promotion_result result);

#endif

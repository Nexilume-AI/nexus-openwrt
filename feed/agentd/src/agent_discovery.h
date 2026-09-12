#ifndef NEXUS_AGENT_DISCOVERY_H
#define NEXUS_AGENT_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_DISCOVERY_INSTANCE_LEN 65
#define AGENT_DISCOVERY_ROUTER_ID_LEN 65
#define AGENT_DISCOVERY_DOMAIN_ID_LEN 128
#define AGENT_DISCOVERY_HOSTNAME_LEN 256
#define AGENT_DISCOVERY_IPV4_LEN 16
#define AGENT_DISCOVERY_IFACE_LEN 32
#define AGENT_DISCOVERY_PATH_LEN 32
#define AGENT_DISCOVERY_LAST_UPDATE_LEN 32

#define AGENT_DISCOVERY_PROTOCOL_VERSION 1U
#define AGENT_DISCOVERY_MIN_TTL_SECONDS 5U
#define AGENT_DISCOVERY_MAX_TTL_SECONDS 3600U

enum agent_discovery_result {
    AGENT_DISCOVERY_OK = 0,
    AGENT_DISCOVERY_INVALID = -1,
    AGENT_DISCOVERY_FULL = -2,
    AGENT_DISCOVERY_NO_MEMORY = -3,
    AGENT_DISCOVERY_NOT_FOUND = -4
};

struct agent_discovery_observation {
    char instance[AGENT_DISCOVERY_INSTANCE_LEN];
    char router_id[AGENT_DISCOVERY_ROUTER_ID_LEN];
    char domain_id[AGENT_DISCOVERY_DOMAIN_ID_LEN];
    char hostname[AGENT_DISCOVERY_HOSTNAME_LEN];
    char ipv4[AGENT_DISCOVERY_IPV4_LEN];
    char iface[AGENT_DISCOVERY_IFACE_LEN];
    char registration_path[AGENT_DISCOVERY_PATH_LEN];
    char last_update[AGENT_DISCOVERY_LAST_UPDATE_LEN];
    uint32_t protocol_version;
    uint16_t port;
    uint32_t ttl_seconds;
    uint8_t txt_fields;
};

struct agent_discovery_candidate {
    struct agent_discovery_observation observation;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint64_t expires_at_ms;
    struct agent_discovery_candidate *next;
};

struct agent_discovery_table {
    struct agent_discovery_candidate *head;
    size_t count;
    size_t max_candidates;
    uint64_t generation;
    uint64_t accepted;
    uint64_t rejected;
    uint64_t expired;
    uint64_t self_suppressed;
};

void agent_discovery_table_init(
    struct agent_discovery_table *table,
    size_t max_candidates
);
void agent_discovery_table_destroy(struct agent_discovery_table *table);

void agent_discovery_observation_init(
    struct agent_discovery_observation *observation
);
bool agent_discovery_observation_add_txt(
    struct agent_discovery_observation *observation,
    const char *txt
);
bool agent_discovery_observation_valid(
    const struct agent_discovery_observation *observation
);

enum agent_discovery_result agent_discovery_table_observe(
    struct agent_discovery_table *table,
    const struct agent_discovery_observation *observation,
    const char *local_router_id,
    uint64_t now_ms
);
size_t agent_discovery_table_prune(
    struct agent_discovery_table *table,
    uint64_t now_ms
);

const struct agent_discovery_candidate *agent_discovery_table_first(
    const struct agent_discovery_table *table
);
const struct agent_discovery_candidate *agent_discovery_table_find(
    const struct agent_discovery_table *table,
    const char *router_id
);

#endif

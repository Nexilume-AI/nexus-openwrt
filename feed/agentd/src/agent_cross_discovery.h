#ifndef NEXUS_AGENT_CROSS_DISCOVERY_H
#define NEXUS_AGENT_CROSS_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_CROSS_OWNER_LEN 192U
#define AGENT_CROSS_HOSTNAME_LEN 256U
#define AGENT_CROSS_ROUTER_ID_LEN 65U
#define AGENT_CROSS_DOMAIN_ID_LEN 128U
#define AGENT_CROSS_CARD_URI_LEN 384U
#define AGENT_CROSS_IPV4_LEN 16U
#define AGENT_CROSS_MIN_TTL_SECONDS 5U
#define AGENT_CROSS_MAX_TTL_SECONDS 3600U
#define AGENT_CROSS_MAX_DNS_PACKET 4096U
#define AGENT_CROSS_SVCB_TYPE 64U

/* Private-use SvcParamKeys for the feature-gated Nexus experiment. */
#define AGENT_CROSS_KEY_ROUTER_ID 65400U
#define AGENT_CROSS_KEY_DOMAIN_ID 65401U
#define AGENT_CROSS_KEY_AGENT_CARD 65402U

enum agent_cross_parse_result {
    AGENT_CROSS_PARSE_OK = 0,
    AGENT_CROSS_PARSE_NO_DATA,
    AGENT_CROSS_PARSE_INSECURE,
    AGENT_CROSS_PARSE_TRUNCATED,
    AGENT_CROSS_PARSE_RCODE,
    AGENT_CROSS_PARSE_MALFORMED
};

struct agent_cross_candidate {
    char owner[AGENT_CROSS_OWNER_LEN];
    char target[AGENT_CROSS_HOSTNAME_LEN];
    char router_id[AGENT_CROSS_ROUTER_ID_LEN];
    char domain_id[AGENT_CROSS_DOMAIN_ID_LEN];
    char agent_card_uri[AGENT_CROSS_CARD_URI_LEN];
    char ipv4_hint[AGENT_CROSS_IPV4_LEN];
    uint16_t priority;
    uint16_t port;
    uint32_t ttl_seconds;
    uint32_t unknown_optional_keys;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint64_t expires_at_ms;
    struct agent_cross_candidate *next;
};

struct agent_cross_table {
    struct agent_cross_candidate *head;
    size_t count;
    size_t max_candidates;
    uint64_t generation;
    uint64_t records_accepted;
    uint64_t records_rejected;
    uint64_t records_incompatible;
    uint64_t dnssec_rejected;
    uint64_t candidates_expired;
};

void agent_cross_table_init(
    struct agent_cross_table *table,
    size_t max_candidates
);
void agent_cross_table_destroy(struct agent_cross_table *table);

bool agent_cross_owner_from_domain(
    const char *domain,
    char *owner,
    size_t owner_capacity
);

size_t agent_cross_build_query(
    uint8_t *packet,
    size_t capacity,
    uint16_t query_id,
    const char *owner
);

enum agent_cross_parse_result agent_cross_parse_response(
    struct agent_cross_table *table,
    const uint8_t *packet,
    size_t packet_length,
    uint16_t expected_query_id,
    const char *expected_owner,
    uint64_t now_ms,
    uint32_t ttl_cap_seconds
);

size_t agent_cross_table_prune(
    struct agent_cross_table *table,
    uint64_t now_ms
);

const struct agent_cross_candidate *agent_cross_table_first(
    const struct agent_cross_table *table
);

#endif

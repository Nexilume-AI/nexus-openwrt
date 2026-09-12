#ifndef NEXUS_AGENT_PEER_TABLE_H
#define NEXUS_AGENT_PEER_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_PEER_ID_LEN 65
#define AGENT_ROUTER_ID_LEN 65
#define AGENT_DOMAIN_ID_LEN 128
#define AGENT_PEER_ENDPOINT_LEN 256
#define AGENT_PEER_CONNECT_IPV4_LEN 16
#define AGENT_PEER_RELAY_TICKET_LEN 1537

enum peer_table_result {
    PEER_TABLE_OK = 0,
    PEER_TABLE_INVALID = -1,
    PEER_TABLE_FULL = -2,
    PEER_TABLE_DUPLICATE = -3,
    PEER_TABLE_NOT_FOUND = -4,
    PEER_TABLE_NO_MEMORY = -5,
    PEER_TABLE_INVALID_TRANSITION = -6
};

enum agent_peer_role {
    AGENT_PEER_ROLE_DIRECT = 0,
    AGENT_PEER_ROLE_REFLECTOR = 1,
    AGENT_PEER_ROLE_RELAY = 2
};

enum agent_peer_state {
    AGENT_PEER_STATE_CONFIGURED = 0,
    AGENT_PEER_STATE_CONNECTING = 1,
    AGENT_PEER_STATE_ESTABLISHED = 2,
    AGENT_PEER_STATE_STALE = 3,
    AGENT_PEER_STATE_DOWN = 4
};

struct agent_peer {
    char peer_id[AGENT_PEER_ID_LEN];
    char router_id[AGENT_ROUTER_ID_LEN];
    char domain_id[AGENT_DOMAIN_ID_LEN];
    char endpoint[AGENT_PEER_ENDPOINT_LEN];
    char connect_ipv4[AGENT_PEER_CONNECT_IPV4_LEN];
    char relay_session_ticket[AGENT_PEER_RELAY_TICKET_LEN];
    enum agent_peer_role role;
    enum agent_peer_state state;
    uint32_t graceful_restart_seconds;
    uint64_t boot_epoch;
    uint64_t last_sequence;
    uint64_t stale_until_ms;
    size_t learned_routes;
    bool snapshot_receiving;
    /* True only for a Relay assignment obtained from the dedicated
     * Open Mesh Directory endpoint. Cloud Relay peers always keep this false. */
    bool open_mesh;
    uint64_t snapshot_id;
    uint64_t snapshots_completed;
    struct agent_peer *next;
};

struct peer_table {
    struct agent_peer *head;
    size_t count;
    size_t max_peers;
    uint64_t generation;
};

void peer_table_init(struct peer_table *table, size_t max_peers);
void peer_table_destroy(struct peer_table *table);

bool agent_peer_id_valid(const char *identifier, size_t capacity);
bool agent_peer_domain_valid(const char *domain_id);
bool agent_peer_endpoint_valid(const char *endpoint);
bool agent_peer_valid(const struct agent_peer *peer);

enum peer_table_result peer_table_upsert(
    struct peer_table *table,
    const struct agent_peer *peer
);

enum peer_table_result peer_table_remove(
    struct peer_table *table,
    const char *peer_id
);

enum peer_table_result peer_table_set_state(
    struct peer_table *table,
    const char *peer_id,
    enum agent_peer_state state
);

/* Refreshes an outbound Relay credential without replacing peer runtime state. */
enum peer_table_result peer_table_set_relay_ticket(
    struct peer_table *table,
    const char *peer_id,
    const char *session_ticket
);

void peer_table_swap(struct peer_table *left, struct peer_table *right);

const struct agent_peer *peer_table_first(const struct peer_table *table);
const struct agent_peer *peer_table_find(
    const struct peer_table *table,
    const char *peer_id
);

const char *agent_peer_role_name(enum agent_peer_role role);
const char *agent_peer_state_name(enum agent_peer_state state);

#endif

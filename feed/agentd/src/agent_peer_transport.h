#ifndef NEXUS_AGENT_PEER_TRANSPORT_H
#define NEXUS_AGENT_PEER_TRANSPORT_H

#include "agent_arpx_protocol.h"
#include "agent_relay_tunnel.h"
#include "peer_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_PEER_TRANSPORT_FILE_LEN 256U
#define AGENT_PEER_TRANSPORT_ERROR_LEN 128U
#define AGENT_PEER_TRANSPORT_QUEUE_CAPACITY 32U
#define AGENT_PEER_TUNNEL_QUEUE_CAPACITY 8U

typedef bool (*agent_peer_message_handler)(
    void *context,
    const char *peer_id,
    const struct agent_arpx_message *message,
    uint64_t now_ms
);

typedef void (*agent_peer_session_handler)(
    void *context,
    const char *peer_id,
    bool session_up,
    uint64_t now_ms
);

typedef bool (*agent_peer_tunnel_handler)(
    void *context,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message,
    uint64_t now_ms
);

struct agent_peer_transport_config {
    bool enabled;
    /* Open Mesh admits every directly discovered/configured Router while
     * keeping Relay certificate verification strict.  TLS remains mandatory;
     * only the external CA/domain admission gate is relaxed for direct peers. */
    bool open_mesh;
    bool deterministic_roles;
    bool relay_tunnel_enabled;
    char local_router_id[AGENT_ROUTER_ID_LEN];
    char local_domain_id[AGENT_DOMAIN_ID_LEN];
    char ca_file[AGENT_PEER_TRANSPORT_FILE_LEN];
    char client_cert_file[AGENT_PEER_TRANSPORT_FILE_LEN];
    char client_key_file[AGENT_PEER_TRANSPORT_FILE_LEN];
    uint32_t heartbeat_ms;
    uint32_t connect_timeout_ms;
    uint32_t open_timeout_ms;
    uint32_t heartbeat_miss_limit;
    uint32_t initial_backoff_ms;
    uint32_t max_backoff_ms;
    uint32_t tick_ms;
    agent_peer_message_handler message_handler;
    agent_peer_session_handler session_handler;
    agent_peer_tunnel_handler tunnel_handler;
    void *event_context;
};

struct agent_peer_transport_status {
    bool configured;
    bool eligible;
    bool session_up;
    const char *phase;
    const char *direction;
    uint64_t remote_boot_epoch;
    uint64_t remote_sequence;
    uint64_t local_sequence;
    uint64_t tcp_connects;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t protocol_errors;
    uint64_t reconnects;
    bool relay_tunnel_enabled;
    bool relay_tunnel_up;
    size_t relay_tunnel_streams;
    uint64_t relay_tunnel_frames_sent;
    uint64_t relay_tunnel_frames_received;
    uint64_t relay_tunnel_protocol_errors;
    char last_error[AGENT_PEER_TRANSPORT_ERROR_LEN];
};

struct agent_peer_transport_stats {
    bool enabled;
    size_t slots;
    size_t eligible;
    size_t sessions_up;
    uint64_t tcp_connects;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t failures;
    bool relay_tunnel_enabled;
    size_t relay_tunnels_up;
    size_t relay_tunnel_streams;
    uint64_t relay_tunnel_frames_sent;
    uint64_t relay_tunnel_frames_received;
    uint64_t relay_tunnel_protocol_errors;
};

struct agent_peer_transport_manager;

struct agent_peer_transport_manager *agent_peer_transport_create(
    const struct agent_peer_transport_config *config,
    struct peer_table *peers,
    uint64_t (*now_ms)(void),
    char *error,
    size_t error_capacity
);

bool agent_peer_transport_reload(
    struct agent_peer_transport_manager *manager,
    struct peer_table *peers,
    char *error,
    size_t error_capacity
);

void agent_peer_transport_destroy(
    struct agent_peer_transport_manager *manager
);

bool agent_peer_transport_get_status(
    const struct agent_peer_transport_manager *manager,
    const char *peer_id,
    struct agent_peer_transport_status *status
);

/* Refreshes an established Relay session in-band and updates reconnect state. */
bool agent_peer_transport_set_relay_ticket(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    const char *session_ticket
);

void agent_peer_transport_get_stats(
    const struct agent_peer_transport_manager *manager,
    struct agent_peer_transport_stats *stats
);

uint64_t agent_peer_transport_boot_epoch(
    const struct agent_peer_transport_manager *manager
);

bool agent_peer_transport_send(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    const struct agent_arpx_message *message
);

bool agent_peer_transport_tunnel_open(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    struct agent_relay_tunnel_message *message
);

bool agent_peer_transport_tunnel_send(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message
);

bool agent_peer_transport_tunnel_send_credit(
    struct agent_peer_transport_manager *manager, const char *peer_id,
    uint32_t stream_id, uint32_t *credit);

#endif

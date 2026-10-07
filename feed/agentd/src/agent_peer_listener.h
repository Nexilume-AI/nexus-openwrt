#ifndef NEXUS_AGENT_PEER_LISTENER_H
#define NEXUS_AGENT_PEER_LISTENER_H

#include "agent_peer_transport.h"
#include "peer_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_PEER_LISTEN_IPV4_LEN 16U

struct agent_peer_listener_config {
    bool enabled;
    char listen_ipv4[AGENT_PEER_LISTEN_IPV4_LEN];
    uint16_t listen_port;
    uint32_t max_connections;
    uint64_t boot_epoch;
    struct agent_peer_transport_config session;
};

struct agent_peer_listener_status {
    bool configured;
    bool eligible;
    bool session_up;
    const char *phase;
    const char *direction;
    uint64_t local_sequence;
    uint64_t accepts;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t protocol_errors;
    uint64_t reconnects;
    bool invoke_tunnel_enabled;
    bool invoke_tunnel_up;
    size_t invoke_tunnel_streams;
    uint64_t invoke_tunnel_frames_sent;
    uint64_t invoke_tunnel_frames_received;
    uint64_t invoke_tunnel_protocol_errors;
    char last_error[AGENT_PEER_TRANSPORT_ERROR_LEN];
};

struct agent_peer_listener_stats {
    bool enabled;
    bool listening;
    char listen_ipv4[AGENT_PEER_LISTEN_IPV4_LEN];
    uint16_t listen_port;
    size_t eligible;
    size_t active_connections;
    size_t sessions_up;
    uint64_t accepts;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t failures;
    uint64_t rejected_connections;
    bool invoke_tunnel_enabled;
    size_t invoke_tunnels_up;
    size_t invoke_tunnel_streams;
    uint64_t invoke_tunnel_frames_sent;
    uint64_t invoke_tunnel_frames_received;
    uint64_t invoke_tunnel_protocol_errors;
};

struct agent_peer_listener;

struct agent_peer_listener *agent_peer_listener_create(
    const struct agent_peer_listener_config *config,
    struct peer_table *peers,
    uint64_t (*now_ms)(void),
    char *error,
    size_t error_capacity
);

bool agent_peer_listener_reload(
    struct agent_peer_listener *listener,
    struct peer_table *peers,
    char *error,
    size_t error_capacity
);

void agent_peer_listener_destroy(struct agent_peer_listener *listener);

bool agent_peer_listener_get_status(
    const struct agent_peer_listener *listener,
    const char *peer_id,
    struct agent_peer_listener_status *status
);

void agent_peer_listener_get_stats(
    const struct agent_peer_listener *listener,
    struct agent_peer_listener_stats *stats
);

bool agent_peer_listener_send(
    struct agent_peer_listener *listener,
    const char *peer_id,
    const struct agent_arpx_message *message
);

bool agent_peer_listener_tunnel_open(
    struct agent_peer_listener *listener,
    const char *peer_id,
    struct agent_relay_tunnel_message *message
);

bool agent_peer_listener_tunnel_send(
    struct agent_peer_listener *listener,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message
);

bool agent_peer_listener_tunnel_send_credit(
    struct agent_peer_listener *manager, const char *peer_id,
    uint32_t stream_id, uint32_t *credit);

#endif

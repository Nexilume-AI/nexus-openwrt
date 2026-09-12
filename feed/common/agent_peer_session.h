#ifndef NEXUS_AGENT_PEER_SESSION_H
#define NEXUS_AGENT_PEER_SESSION_H

#include "agent_arpx_protocol.h"

#include <stdbool.h>
#include <stdint.h>

enum agent_peer_session_state {
    AGENT_PEER_SESSION_CONFIGURED = 0,
    AGENT_PEER_SESSION_CONNECTING = 1,
    AGENT_PEER_SESSION_WAIT_OPEN = 2,
    AGENT_PEER_SESSION_ESTABLISHED = 3,
    AGENT_PEER_SESSION_BACKOFF = 4
};

enum agent_peer_session_result {
    AGENT_PEER_SESSION_OK = 0,
    AGENT_PEER_SESSION_INVALID = -1,
    AGENT_PEER_SESSION_INVALID_STATE = -2,
    AGENT_PEER_SESSION_IDENTITY_MISMATCH = -3,
    AGENT_PEER_SESSION_SEQUENCE_ERROR = -4,
    AGENT_PEER_SESSION_PROTOCOL_ERROR = -5
};

struct agent_peer_session_config {
    char expected_router_id[AGENT_ARPX_ROUTER_ID_LEN];
    char expected_domain_id[AGENT_ARPX_DOMAIN_ID_LEN];
    uint32_t open_timeout_ms;
    uint32_t heartbeat_miss_limit;
    uint32_t initial_backoff_ms;
    uint32_t max_backoff_ms;
};

struct agent_peer_session {
    struct agent_peer_session_config config;
    enum agent_peer_session_state state;
    uint64_t remote_boot_epoch;
    uint64_t last_sequence;
    uint32_t remote_heartbeat_ms;
    uint64_t deadline_ms;
    uint64_t next_action_ms;
    uint32_t current_backoff_ms;
    uint64_t opens_accepted;
    uint64_t heartbeats_accepted;
    uint64_t updates_accepted;
    uint64_t withdrawals_accepted;
    uint64_t snapshot_requests_accepted;
    uint64_t snapshot_ends_accepted;
    uint64_t protocol_errors;
    uint64_t timeouts;
    uint64_t reconnects;
};

bool agent_peer_session_init(
    struct agent_peer_session *session,
    const struct agent_peer_session_config *config
);

enum agent_peer_session_result agent_peer_session_begin(
    struct agent_peer_session *session,
    uint64_t now_ms
);

enum agent_peer_session_result agent_peer_session_transport_ready(
    struct agent_peer_session *session,
    uint64_t now_ms
);

enum agent_peer_session_result agent_peer_session_on_message(
    struct agent_peer_session *session,
    const struct agent_arpx_message *message,
    uint64_t now_ms
);

void agent_peer_session_transport_down(
    struct agent_peer_session *session,
    uint64_t now_ms
);

bool agent_peer_session_tick(
    struct agent_peer_session *session,
    uint64_t now_ms
);

const char *agent_peer_session_state_name(enum agent_peer_session_state state);

#endif

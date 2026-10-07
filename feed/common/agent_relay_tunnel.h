#ifndef NEXUS_AGENT_RELAY_TUNNEL_H
#define NEXUS_AGENT_RELAY_TUNNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_RELAY_TUNNEL_MAGIC 0x4e585431U
#define AGENT_RELAY_TUNNEL_VERSION 1U
#define AGENT_RELAY_TUNNEL_HEADER_SIZE 24U
#define AGENT_RELAY_TUNNEL_MAX_DATA 4096U
#define AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE \
    (AGENT_RELAY_TUNNEL_HEADER_SIZE + AGENT_RELAY_TUNNEL_MAX_DATA)
#define AGENT_RELAY_TUNNEL_MAX_STREAMS 32U
#define AGENT_RELAY_TUNNEL_RESET_HISTORY (AGENT_RELAY_TUNNEL_MAX_STREAMS * 2U)
#define AGENT_RELAY_TUNNEL_ROUTER_ID_LEN 65U
#define AGENT_RELAY_TUNNEL_INTENT_LEN 128U
#define AGENT_RELAY_TUNNEL_TASK_ID_LEN 65U
#define AGENT_RELAY_TUNNEL_AGENT_ID_LEN 256U
#define AGENT_RELAY_TUNNEL_TENANT_LEN 64U
#define AGENT_RELAY_TUNNEL_REGION_LEN 32U
#define AGENT_RELAY_TUNNEL_ASSERTION_LEN 2049U
#define AGENT_RELAY_TUNNEL_MIN_WINDOW 4096U
#define AGENT_RELAY_TUNNEL_MAX_WINDOW 1048576U

#define AGENT_RELAY_TUNNEL_PATH "/relay/invoke/v1"
#define AGENT_RELAY_TUNNEL_CONTENT_TYPE \
    "application/vnd.nexus.relay-tunnel.v1"

enum agent_relay_tunnel_type {
    AGENT_RELAY_TUNNEL_OPEN = 1,
    AGENT_RELAY_TUNNEL_ACCEPT = 2,
    AGENT_RELAY_TUNNEL_DATA = 3,
    AGENT_RELAY_TUNNEL_END = 4,
    AGENT_RELAY_TUNNEL_RESET = 5,
    AGENT_RELAY_TUNNEL_WINDOW_UPDATE = 6,
    AGENT_RELAY_TUNNEL_PING = 7,
    AGENT_RELAY_TUNNEL_PONG = 8,
    AGENT_RELAY_TUNNEL_RESPONSE_START = 9
};

enum agent_relay_tunnel_result {
    AGENT_RELAY_TUNNEL_OK = 0,
    AGENT_RELAY_TUNNEL_INVALID_ARGUMENT = -1,
    AGENT_RELAY_TUNNEL_INVALID_FRAME = -2,
    AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL = -3,
    AGENT_RELAY_TUNNEL_INVALID_STATE = -4,
    AGENT_RELAY_TUNNEL_STREAM_LIMIT = -5,
    AGENT_RELAY_TUNNEL_FLOW_CONTROL = -6,
    AGENT_RELAY_TUNNEL_SEQUENCE_ERROR = -7,
    AGENT_RELAY_TUNNEL_CALLBACK_ERROR = -8
};

struct agent_relay_tunnel_message {
    enum agent_relay_tunnel_type type;
    uint32_t stream_id;
    uint64_t sequence;
    bool streaming;
    uint8_t hop_limit;
    uint64_t max_cost_microunits;
    uint32_t max_latency_ms;
    uint16_t status_code;
    uint16_t reset_code;
    uint32_t credit_bytes;
    char target_router_id[AGENT_RELAY_TUNNEL_ROUTER_ID_LEN];
    char intent_class[AGENT_RELAY_TUNNEL_INTENT_LEN];
    char task_id[AGENT_RELAY_TUNNEL_TASK_ID_LEN];
    char source_agent[AGENT_RELAY_TUNNEL_AGENT_ID_LEN];
    char target_agent[AGENT_RELAY_TUNNEL_AGENT_ID_LEN];
    char tenant[AGENT_RELAY_TUNNEL_TENANT_LEN];
    char region[AGENT_RELAY_TUNNEL_REGION_LEN];
    /* Optional P6.1 source-router assertion. Transit routers preserve it
     * byte-for-byte; only the final target router verifies it. */
    char forwarding_assertion[AGENT_RELAY_TUNNEL_ASSERTION_LEN];
    uint8_t data[AGENT_RELAY_TUNNEL_MAX_DATA];
    size_t data_length;
};

struct agent_relay_tunnel_ingress {
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t used;
    size_t expected;
};

typedef bool (*agent_relay_tunnel_frame_callback)(
    const uint8_t *frame,
    size_t frame_size,
    void *context
);

enum agent_relay_mux_stream_state {
    AGENT_RELAY_MUX_STREAM_FREE = 0,
    AGENT_RELAY_MUX_STREAM_OPEN_SENT,
    AGENT_RELAY_MUX_STREAM_OPEN_RECEIVED,
    AGENT_RELAY_MUX_STREAM_ESTABLISHED
};

struct agent_relay_mux_stream {
    enum agent_relay_mux_stream_state state;
    uint32_t stream_id;
    uint64_t next_send_sequence;
    uint64_t next_receive_sequence;
    uint32_t send_credit;
    uint32_t receive_credit;
    bool local_closed;
    bool remote_closed;
};

/* Credit already granted before a local RESET can still be in flight. Keep
 * only sequence/credit, never request bodies or an active application slot. */
struct agent_relay_reset_drain {
    uint32_t stream_id;
    uint64_t next_receive_sequence;
    uint32_t receive_credit;
};

struct agent_relay_mux {
    bool local_odd;
    uint32_t next_stream_id;
    uint32_t largest_remote_stream_id;
    uint32_t initial_window;
    size_t stream_limit;
    struct agent_relay_mux_stream streams[AGENT_RELAY_TUNNEL_MAX_STREAMS];
    size_t active_streams;
    struct agent_relay_reset_drain reset_drains[AGENT_RELAY_TUNNEL_RESET_HISTORY];
    size_t next_reset_drain;
    uint64_t opened_local;
    uint64_t opened_remote;
    uint64_t frames_sent;
    uint64_t frames_received;
    uint64_t resets;
    uint64_t protocol_errors;
};

bool agent_relay_mux_send_credit(
    const struct agent_relay_mux *mux, uint32_t stream_id, uint32_t *credit);

enum agent_relay_tunnel_result agent_relay_tunnel_message_validate(
    const struct agent_relay_tunnel_message *message
);

enum agent_relay_tunnel_result agent_relay_tunnel_frame_encode(
    const struct agent_relay_tunnel_message *message,
    uint8_t *output,
    size_t capacity,
    size_t *written
);

enum agent_relay_tunnel_result agent_relay_tunnel_frame_decode(
    const uint8_t *frame,
    size_t frame_size,
    struct agent_relay_tunnel_message *message
);

void agent_relay_tunnel_ingress_init(
    struct agent_relay_tunnel_ingress *ingress
);

enum agent_relay_tunnel_result agent_relay_tunnel_ingress_feed(
    struct agent_relay_tunnel_ingress *ingress,
    const uint8_t *data,
    size_t size,
    agent_relay_tunnel_frame_callback callback,
    void *context,
    size_t *frames_emitted
);

bool agent_relay_mux_init(
    struct agent_relay_mux *mux,
    bool local_odd,
    size_t stream_limit,
    uint32_t initial_window
);

enum agent_relay_tunnel_result agent_relay_mux_open(
    struct agent_relay_mux *mux,
    struct agent_relay_tunnel_message *message
);

enum agent_relay_tunnel_result agent_relay_mux_on_send(
    struct agent_relay_mux *mux,
    const struct agent_relay_tunnel_message *message
);

enum agent_relay_tunnel_result agent_relay_mux_on_receive(
    struct agent_relay_mux *mux,
    const struct agent_relay_tunnel_message *message
);

const char *agent_relay_tunnel_type_name(enum agent_relay_tunnel_type type);

#endif

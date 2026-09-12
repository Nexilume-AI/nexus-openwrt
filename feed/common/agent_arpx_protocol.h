#ifndef NEXUS_AGENT_ARPX_PROTOCOL_H
#define NEXUS_AGENT_ARPX_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define AGENT_ARPX_VERSION 1U
#define AGENT_ARPX_FRAME_HEADER_SIZE 4U
#define AGENT_ARPX_MAX_FRAME_SIZE 4096U
#define AGENT_ARPX_ROUTER_ID_LEN 65U
#define AGENT_ARPX_DOMAIN_ID_LEN 128U
#define AGENT_ARPX_MIN_HEARTBEAT_MS 1000U
#define AGENT_ARPX_MAX_HEARTBEAT_MS 60000U
#define AGENT_ARPX_ROUTE_ID_LEN 33U
#define AGENT_ARPX_INTENT_LEN 128U
#define AGENT_ARPX_URI_LEN 256U
#define AGENT_ARPX_TENANT_LEN 64U
#define AGENT_ARPX_REGION_LEN 32U
#define AGENT_ARPX_MAX_PATH 8U
#define AGENT_ARPX_MAX_REMAINING_LEASE_MS 3600000U

enum agent_arpx_message_type {
    AGENT_ARPX_OPEN = 1,
    AGENT_ARPX_CAPABILITY_UPDATE = 2,
    AGENT_ARPX_CAPABILITY_WITHDRAW = 3,
    AGENT_ARPX_HEARTBEAT = 4,
    AGENT_ARPX_SNAPSHOT_REQUEST = 5,
    AGENT_ARPX_SNAPSHOT_END = 6,
    AGENT_ARPX_ERROR = 7
};

enum agent_arpx_result {
    AGENT_ARPX_OK = 0,
    AGENT_ARPX_INVALID_ARGUMENT = -1,
    AGENT_ARPX_INVALID_MESSAGE = -2,
    AGENT_ARPX_BUFFER_TOO_SMALL = -3,
    AGENT_ARPX_INVALID_FRAME = -4,
    AGENT_ARPX_UNSUPPORTED = -5
};

struct agent_arpx_message {
    uint32_t version;
    enum agent_arpx_message_type type;
    char router_id[AGENT_ARPX_ROUTER_ID_LEN];
    char domain_id[AGENT_ARPX_DOMAIN_ID_LEN];
    uint64_t boot_epoch;
    uint64_t sequence;
    uint32_t heartbeat_ms;
    char route_id[AGENT_ARPX_ROUTE_ID_LEN];
    char intent[AGENT_ARPX_INTENT_LEN];
    uint32_t capability_version;
    char origin[AGENT_ARPX_URI_LEN];
    char endpoint[AGENT_ARPX_URI_LEN];
    char tenant[AGENT_ARPX_TENANT_LEN];
    char region[AGENT_ARPX_REGION_LEN];
    uint64_t cost_microunits;
    uint32_t latency_ms;
    uint8_t trust_level;
    uint16_t load_permille;
    uint32_t remaining_lease_ms;
    uint64_t snapshot_id;
    uint8_t path_length;
    char path[AGENT_ARPX_MAX_PATH][AGENT_ARPX_ROUTER_ID_LEN];
};

enum agent_arpx_result agent_arpx_message_validate(
    const struct agent_arpx_message *message
);

enum agent_arpx_result agent_arpx_frame_encode(
    const struct agent_arpx_message *message,
    uint8_t *output,
    size_t capacity,
    size_t *written
);

enum agent_arpx_result agent_arpx_frame_decode(
    const uint8_t *frame,
    size_t frame_size,
    struct agent_arpx_message *message
);

const char *agent_arpx_message_type_name(enum agent_arpx_message_type type);

#endif

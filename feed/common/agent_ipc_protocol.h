#ifndef NEXUS_AGENT_IPC_PROTOCOL_H
#define NEXUS_AGENT_IPC_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_IPC_MAGIC 0x4e584152U
#define AGENT_IPC_VERSION 13U
#define AGENT_IPC_MIN_VERSION 8U
#define AGENT_IPC_HEADER_SIZE 16U
#define AGENT_IPC_MAX_INVOKE_BODY 16384U
#define AGENT_IPC_MAX_STREAM_DATA 4096U
#define AGENT_IPC_MAX_STREAM_BYTES 1048576U
#define AGENT_IPC_MAX_FRAME_SIZE 24576U
#define AGENT_IPC_MAX_CANDIDATES 4U

#define AGENT_IPC_ROUTE_ID_LEN 33U
#define AGENT_IPC_INTENT_LEN 128U
#define AGENT_IPC_URI_LEN 256U
#define AGENT_IPC_TENANT_LEN 64U
#define AGENT_IPC_REGION_LEN 32U
#define AGENT_IPC_SOURCE_LEN 16U
#define AGENT_IPC_ERROR_LEN 128U
#define AGENT_IPC_ROUTER_ID_LEN 65U
#define AGENT_IPC_PEER_ID_LEN 65U
#define AGENT_IPC_CONTENT_TYPE_LEN 128U
#define AGENT_IPC_TASK_ID_LEN 65U
#define AGENT_IPC_AGENT_ID_LEN 256U
#define AGENT_IPC_FORWARDING_ASSERTION_LEN 2049U
#define AGENT_IPC_POLICY_ID_LEN 65U
#define AGENT_IPC_IPV6_LEN 46U
#define AGENT_IPC_CLOUD_NAME_LEN 256U
#define AGENT_IPC_MANIFEST_DIGEST_LEN 65U
#define AGENT_IPC_TOOL_NAME_LEN 96U
#define AGENT_IPC_TOOL_TITLE_LEN 128U
#define AGENT_IPC_TOOL_DESCRIPTION_LEN 512U
#define AGENT_IPC_TOOL_INPUT_SCHEMA_LEN 2048U

enum agent_ipc_computer_requirement {
    AGENT_IPC_COMPUTER_DISABLED = 0,
    AGENT_IPC_COMPUTER_OPTIONAL = 1,
    AGENT_IPC_COMPUTER_REQUIRED = 2
};

enum agent_ipc_mobile_requirement {
    AGENT_IPC_MOBILE_DISABLED = 0,
    AGENT_IPC_MOBILE_OPTIONAL = 1,
    AGENT_IPC_MOBILE_REQUIRED = 2
};

#define AGENT_IPC_MOBILE_OBSERVE        0x0001U
#define AGENT_IPC_MOBILE_SCREEN_CAPTURE 0x0002U
#define AGENT_IPC_MOBILE_TAP            0x0004U
#define AGENT_IPC_MOBILE_TYPE_TEXT      0x0008U
#define AGENT_IPC_MOBILE_SWIPE          0x0010U
#define AGENT_IPC_MOBILE_PRESS_BACK     0x0020U
#define AGENT_IPC_MOBILE_OPEN_APP       0x0040U
#define AGENT_IPC_MOBILE_WAIT_FOR_STATE 0x0080U
#define AGENT_IPC_MOBILE_ALL            0x00ffU

#define AGENT_IPC_WORKSPACE_CONNECTION_LIST   0x0001U
#define AGENT_IPC_WORKSPACE_CONNECTION_CREATE 0x0002U
#define AGENT_IPC_WORKSPACE_CONNECTION_UPDATE 0x0004U
#define AGENT_IPC_WORKSPACE_CONNECTION_DELETE 0x0008U
#define AGENT_IPC_WORKSPACE_CONNECTION_TEST   0x0010U
#define AGENT_IPC_WORKSPACE_CONNECTION_BIND   0x0020U
#define AGENT_IPC_WORKSPACE_FILES_LIST        0x0040U
#define AGENT_IPC_WORKSPACE_FILES_READ        0x0080U
#define AGENT_IPC_WORKSPACE_FILES_WRITE       0x0100U
#define AGENT_IPC_WORKSPACE_COMMAND_EXECUTE   0x0200U
#define AGENT_IPC_WORKSPACE_BROWSER_CONTROL   0x0400U
#define AGENT_IPC_WORKSPACE_ALL               0x07ffU

enum agent_ipc_message_type {
    AGENT_IPC_LOOKUP_REQUEST = 1,
    AGENT_IPC_LOOKUP_RESPONSE = 2,
    AGENT_IPC_ERROR_RESPONSE = 3,
    AGENT_IPC_CANDIDATES_REQUEST = 4,
    AGENT_IPC_CANDIDATES_RESPONSE = 5,
    AGENT_IPC_INVOKE_REQUEST = 6,
    AGENT_IPC_INVOKE_RESPONSE = 7,
    AGENT_IPC_STREAM_REQUEST = 8,
    AGENT_IPC_STREAM_START = 9,
    AGENT_IPC_STREAM_DATA = 10,
    AGENT_IPC_STREAM_END = 11,
    AGENT_IPC_REGISTER_REQUEST = 12,
    AGENT_IPC_RENEW_REQUEST = 13,
    AGENT_IPC_UNREGISTER_REQUEST = 14,
    AGENT_IPC_LEASE_RESPONSE = 15
};

#define AGENT_IPC_RENEW_LATENCY 0x01U
#define AGENT_IPC_RENEW_LOAD 0x02U
#define AGENT_IPC_RENEW_HEALTH 0x04U

enum agent_ipc_result {
    AGENT_IPC_OK = 0,
    AGENT_IPC_INVALID = -1,
    AGENT_IPC_TOO_SMALL = -2,
    AGENT_IPC_WRONG_TYPE = -3
};

struct agent_ipc_header {
    uint16_t version;
    uint16_t type;
    uint32_t request_id;
    uint32_t payload_length;
};

struct agent_ipc_lookup_request {
    uint32_t request_id;
    char intent[AGENT_IPC_INTENT_LEN];
    uint32_t version;
    char tenant[AGENT_IPC_TENANT_LEN];
    char region[AGENT_IPC_REGION_LEN];
    char source_agent[AGENT_IPC_AGENT_ID_LEN];
    char target_agent[AGENT_IPC_URI_LEN];
    char public_ipv6[AGENT_IPC_IPV6_LEN];
    uint64_t max_cost_microunits;
    uint32_t max_latency_ms;
    uint8_t min_trust_level;
};

struct agent_ipc_lookup_response {
    uint32_t request_id;
    bool found;
    uint64_t generation;
    uint64_t score;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    char origin[AGENT_IPC_URI_LEN];
    char endpoint[AGENT_IPC_URI_LEN];
    char source[AGENT_IPC_SOURCE_LEN];
    bool relay;
    char relay_peer_id[AGENT_IPC_PEER_ID_LEN];
    char target_router_id[AGENT_IPC_ROUTER_ID_LEN];
    char policy_id[AGENT_IPC_POLICY_ID_LEN];
    uint32_t tenant_max_inflight;
    uint32_t tenant_rate_per_second;
    uint32_t tenant_rate_burst;
};

struct agent_ipc_candidates_request {
    struct agent_ipc_lookup_request lookup;
    uint8_t max_candidates;
};

struct agent_ipc_route_candidate {
    uint64_t score;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    char origin[AGENT_IPC_URI_LEN];
    char endpoint[AGENT_IPC_URI_LEN];
    char source[AGENT_IPC_SOURCE_LEN];
    bool relay;
    char relay_peer_id[AGENT_IPC_PEER_ID_LEN];
    char target_router_id[AGENT_IPC_ROUTER_ID_LEN];
};

struct agent_ipc_invoke_request {
    uint32_t request_id;
    uint32_t timeout_ms;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    char relay_peer_id[AGENT_IPC_PEER_ID_LEN];
    char target_router_id[AGENT_IPC_ROUTER_ID_LEN];
    char intent[AGENT_IPC_INTENT_LEN];
    char task_id[AGENT_IPC_TASK_ID_LEN];
    char source_agent[AGENT_IPC_AGENT_ID_LEN];
    char target_agent[AGENT_IPC_URI_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char region[AGENT_IPC_REGION_LEN];
    char forwarding_assertion[AGENT_IPC_FORWARDING_ASSERTION_LEN];
    uint8_t hop_limit;
    uint64_t max_cost_microunits;
    uint32_t max_latency_ms;
    uint8_t body[AGENT_IPC_MAX_INVOKE_BODY];
    size_t body_length;
    bool streaming;
};

struct agent_ipc_invoke_response {
    uint32_t request_id;
    uint16_t status_code;
    char content_type[AGENT_IPC_CONTENT_TYPE_LEN];
    uint8_t body[AGENT_IPC_MAX_INVOKE_BODY];
    size_t body_length;
};

struct agent_ipc_stream_start {
    uint32_t request_id;
    uint16_t status_code;
    char content_type[AGENT_IPC_CONTENT_TYPE_LEN];
};

struct agent_ipc_stream_data {
    uint32_t request_id;
    uint8_t data[AGENT_IPC_MAX_STREAM_DATA];
    size_t data_length;
};

struct agent_ipc_stream_end {
    uint32_t request_id;
    uint64_t total_bytes;
};

struct agent_ipc_candidates_response {
    uint32_t request_id;
    uint64_t generation;
    uint8_t count;
    struct agent_ipc_route_candidate candidates[AGENT_IPC_MAX_CANDIDATES];
    char policy_id[AGENT_IPC_POLICY_ID_LEN];
    uint32_t tenant_max_inflight;
    uint32_t tenant_rate_per_second;
    uint32_t tenant_rate_burst;
};

struct agent_ipc_error_response {
    uint32_t request_id;
    uint32_t code;
    char message[AGENT_IPC_ERROR_LEN];
};

struct agent_ipc_register_request {
    uint32_t request_id;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    char intent[AGENT_IPC_INTENT_LEN];
    uint32_t version;
    char origin[AGENT_IPC_URI_LEN];
    char endpoint[AGENT_IPC_URI_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char region[AGENT_IPC_REGION_LEN];
    uint64_t cost_microunits;
    uint32_t latency_ms;
    uint8_t trust_level;
    uint16_t load_permille;
    uint8_t hop_count;
    uint32_t lease_seconds;
    bool request_public_ipv6;
    struct {
        bool present;
        bool publish;
        bool tool_present;
        char agent_name[AGENT_IPC_CLOUD_NAME_LEN];
        char manifest_digest[AGENT_IPC_MANIFEST_DIGEST_LEN];
        char tool_name[AGENT_IPC_TOOL_NAME_LEN];
        char tool_title[AGENT_IPC_TOOL_TITLE_LEN];
        char tool_description[AGENT_IPC_TOOL_DESCRIPTION_LEN];
        char tool_input_schema[AGENT_IPC_TOOL_INPUT_SCHEMA_LEN];
        bool task;
        bool resumable;
        bool continuable;
        uint8_t recovery_protocol;
        bool demo;
        bool chat;
        bool interactive;
        bool computer_present;
        uint8_t computer_requirement;
        uint16_t workspace_capabilities;
        bool mobile_present;
        uint8_t mobile_requirement;
        uint16_t mobile_capabilities;
        uint16_t mobile_scopes;
    } cloud;
};

struct agent_ipc_renew_request {
    uint32_t request_id;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    uint32_t lease_seconds;
    uint32_t latency_ms;
    uint16_t load_permille;
    uint8_t update_flags;
    bool healthy;
    char requester_agent[AGENT_IPC_AGENT_ID_LEN];
};

struct agent_ipc_unregister_request {
    uint32_t request_id;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    char requester_agent[AGENT_IPC_AGENT_ID_LEN];
};

struct agent_ipc_lease_response {
    uint32_t request_id;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
    uint64_t generation;
    uint32_t lease_seconds;
    bool removed;
    char public_ipv6[AGENT_IPC_IPV6_LEN];
};

enum agent_ipc_result agent_ipc_decode_header(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_header *header
);

enum agent_ipc_result agent_ipc_encode_lookup_request(
    const struct agent_ipc_lookup_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_lookup_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_lookup_request *request
);

enum agent_ipc_result agent_ipc_encode_lookup_response(
    const struct agent_ipc_lookup_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_lookup_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_lookup_response *response
);

enum agent_ipc_result agent_ipc_encode_candidates_request(
    const struct agent_ipc_candidates_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_candidates_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_candidates_request *request
);

enum agent_ipc_result agent_ipc_encode_candidates_response(
    const struct agent_ipc_candidates_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_candidates_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_candidates_response *response
);

enum agent_ipc_result agent_ipc_encode_error_response(
    const struct agent_ipc_error_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_error_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_error_response *response
);

enum agent_ipc_result agent_ipc_encode_invoke_request(
    const struct agent_ipc_invoke_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_invoke_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_request *request
);

enum agent_ipc_result agent_ipc_encode_stream_request(
    const struct agent_ipc_invoke_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_stream_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_request *request
);

enum agent_ipc_result agent_ipc_encode_invoke_response(
    const struct agent_ipc_invoke_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_invoke_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_response *response
);

enum agent_ipc_result agent_ipc_encode_stream_start(
    const struct agent_ipc_stream_start *start,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_stream_start(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_stream_start *start
);

enum agent_ipc_result agent_ipc_encode_stream_data(
    const struct agent_ipc_stream_data *data,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_stream_data(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_stream_data *data
);

enum agent_ipc_result agent_ipc_encode_stream_end(
    const struct agent_ipc_stream_end *end,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_stream_end(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_stream_end *end
);

enum agent_ipc_result agent_ipc_encode_register_request(
    const struct agent_ipc_register_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_register_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_register_request *request
);

enum agent_ipc_result agent_ipc_encode_renew_request(
    const struct agent_ipc_renew_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_renew_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_renew_request *request
);

enum agent_ipc_result agent_ipc_encode_unregister_request(
    const struct agent_ipc_unregister_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_unregister_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_unregister_request *request
);

enum agent_ipc_result agent_ipc_encode_lease_response(
    const struct agent_ipc_lease_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
);

enum agent_ipc_result agent_ipc_decode_lease_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_lease_response *response
);

#endif

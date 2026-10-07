#ifndef NEXUS_ADAPTER_CODEC_H
#define NEXUS_ADAPTER_CODEC_H

#include "adapter_registry.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum adapter_codec_result {
    ADAPTER_CODEC_OK = 0,
    ADAPTER_CODEC_INVALID_JSON,
    ADAPTER_CODEC_INVALID_REQUEST,
    ADAPTER_CODEC_ROUTE_NOT_FOUND,
    ADAPTER_CODEC_OUT_OF_MEMORY
};

enum adapter_mcp_control_result {
    ADAPTER_MCP_CONTROL_NOT_CONTROL = 0,
    ADAPTER_MCP_CONTROL_HANDLED,
    ADAPTER_MCP_CONTROL_NOTIFICATION,
    ADAPTER_MCP_CONTROL_INVALID,
    ADAPTER_MCP_CONTROL_OUT_OF_MEMORY
};

struct adapter_codec_options {
    enum agent_adapter_protocol protocol;
    const char *authority;
    const char *selector;
    const char *tenant;
    const char *source_agent;
    const char *deadline;
    const char *region;
    const char *run_context_url;
    const char *run_context_token;
    uint32_t hop_limit;
};

struct adapter_normalized_request {
    enum agent_adapter_protocol protocol;
    bool interactive;
    char selector[AGENT_ADAPTER_SELECTOR_LEN];
    char external_id[AGENT_ADAPTER_TASK_ID_LEN];
    char task_id[AGENT_ADAPTER_TASK_ID_LEN];
    struct json_object *request;
    struct json_object *envelope;
};

struct adapter_mcp_stream_state {
    bool finished;
    bool has_progress;
    double progress;
};

/* Decode a complete Nexus SSE event into an MCP JSON-RPC message. A NULL
 * message means a comment or an unrequested progress update was consumed. */
bool adapter_codec_mcp_event(
    const struct adapter_normalized_request *normalized,
    struct adapter_mcp_stream_state *state,
    const char *event, size_t event_length,
    struct json_object **message
);

enum adapter_codec_result adapter_codec_normalize(
    const struct adapter_registry *registry,
    const struct adapter_codec_options *options,
    const char *body,
    size_t body_length,
    struct adapter_normalized_request *normalized
);

enum adapter_mcp_control_result adapter_codec_mcp_control(
    const struct adapter_registry *registry,
    const char *authority,
    const char *body,
    size_t body_length,
    struct json_object **response
);

bool adapter_codec_serialize_envelope(
    struct json_object *envelope,
    const char **serialized,
    size_t *serialized_length
);

struct json_object *adapter_codec_map_response(
    const struct adapter_normalized_request *normalized,
    int gateway_status,
    const char *gateway_content_type,
    const char *body,
    size_t body_length
);

const char *adapter_codec_response_content_type(
    enum agent_adapter_protocol protocol
);

void adapter_codec_request_clear(
    struct adapter_normalized_request *normalized
);

#endif

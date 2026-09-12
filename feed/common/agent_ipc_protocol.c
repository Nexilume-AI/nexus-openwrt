#include "agent_ipc_protocol.h"

#include <string.h>

#define LOOKUP_REQUEST_PAYLOAD_SIZE \
    (4U + 8U + 4U + 1U + AGENT_IPC_INTENT_LEN + \
     AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN + AGENT_IPC_AGENT_ID_LEN + \
     AGENT_IPC_URI_LEN + AGENT_IPC_IPV6_LEN)

#define LOOKUP_RESPONSE_PAYLOAD_SIZE \
    (1U + 8U + 8U + AGENT_IPC_ROUTE_ID_LEN + AGENT_IPC_URI_LEN + \
     AGENT_IPC_URI_LEN + AGENT_IPC_SOURCE_LEN + 1U + \
     AGENT_IPC_PEER_ID_LEN + AGENT_IPC_ROUTER_ID_LEN + \
     AGENT_IPC_POLICY_ID_LEN + 12U)

#define CANDIDATES_REQUEST_PAYLOAD_SIZE (LOOKUP_REQUEST_PAYLOAD_SIZE + 1U)
#define CANDIDATE_ITEM_SIZE \
    (8U + AGENT_IPC_ROUTE_ID_LEN + (2U * AGENT_IPC_URI_LEN) + \
     AGENT_IPC_SOURCE_LEN + 1U + AGENT_IPC_PEER_ID_LEN + \
     AGENT_IPC_ROUTER_ID_LEN)
#define CANDIDATES_RESPONSE_PAYLOAD_SIZE \
    (1U + 8U + (AGENT_IPC_MAX_CANDIDATES * CANDIDATE_ITEM_SIZE) + \
     AGENT_IPC_POLICY_ID_LEN + 12U)

#define ERROR_RESPONSE_PAYLOAD_SIZE (4U + AGENT_IPC_ERROR_LEN)
#define INVOKE_REQUEST_FIXED_PAYLOAD_SIZE \
    (4U + 1U + 8U + 4U + AGENT_IPC_ROUTE_ID_LEN + \
     AGENT_IPC_PEER_ID_LEN + AGENT_IPC_ROUTER_ID_LEN + \
     AGENT_IPC_INTENT_LEN + AGENT_IPC_TASK_ID_LEN + \
     AGENT_IPC_AGENT_ID_LEN + AGENT_IPC_URI_LEN + AGENT_IPC_TENANT_LEN + \
     AGENT_IPC_REGION_LEN + AGENT_IPC_FORWARDING_ASSERTION_LEN + 4U)
#define INVOKE_RESPONSE_FIXED_PAYLOAD_SIZE \
    (2U + AGENT_IPC_CONTENT_TYPE_LEN + 4U)
#define STREAM_START_PAYLOAD_SIZE (2U + AGENT_IPC_CONTENT_TYPE_LEN)
#define STREAM_DATA_FIXED_PAYLOAD_SIZE 4U
#define STREAM_END_PAYLOAD_SIZE 8U
#define REGISTER_REQUEST_V8_PAYLOAD_SIZE \
    (25U + AGENT_IPC_ROUTE_ID_LEN + AGENT_IPC_INTENT_LEN + \
     (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_TENANT_LEN + \
     AGENT_IPC_REGION_LEN)
#define REGISTER_CLOUD_PAYLOAD_SIZE \
    (8U + AGENT_IPC_CLOUD_NAME_LEN + AGENT_IPC_MANIFEST_DIGEST_LEN + \
     AGENT_IPC_TOOL_NAME_LEN + AGENT_IPC_TOOL_TITLE_LEN + \
     AGENT_IPC_TOOL_DESCRIPTION_LEN + AGENT_IPC_TOOL_INPUT_SCHEMA_LEN)
#define REGISTER_REQUEST_V9_PAYLOAD_SIZE \
    (REGISTER_REQUEST_V8_PAYLOAD_SIZE + REGISTER_CLOUD_PAYLOAD_SIZE)
#define REGISTER_REQUEST_V10_PAYLOAD_SIZE \
    (REGISTER_REQUEST_V9_PAYLOAD_SIZE + 4U)
#define REGISTER_REQUEST_V12_PAYLOAD_SIZE \
    (REGISTER_REQUEST_V10_PAYLOAD_SIZE + 6U)
#define REGISTER_REQUEST_PAYLOAD_SIZE \
    (REGISTER_REQUEST_V12_PAYLOAD_SIZE + 2U)
#define RENEW_REQUEST_PAYLOAD_SIZE \
    (12U + AGENT_IPC_ROUTE_ID_LEN + AGENT_IPC_AGENT_ID_LEN)
#define UNREGISTER_REQUEST_PAYLOAD_SIZE \
    (AGENT_IPC_ROUTE_ID_LEN + AGENT_IPC_AGENT_ID_LEN)
#define LEASE_RESPONSE_PAYLOAD_SIZE \
    (13U + AGENT_IPC_ROUTE_ID_LEN + AGENT_IPC_IPV6_LEN)

static void write_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8U);
    target[1] = (uint8_t)value;
}

static void write_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24U);
    target[1] = (uint8_t)(value >> 16U);
    target[2] = (uint8_t)(value >> 8U);
    target[3] = (uint8_t)value;
}

static void write_u64(uint8_t *target, uint64_t value)
{
    write_u32(target, (uint32_t)(value >> 32U));
    write_u32(target + 4U, (uint32_t)value);
}

static uint16_t read_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8U) |
                      (uint16_t)source[1]);
}

static uint32_t read_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
           ((uint32_t)source[1] << 16U) |
           ((uint32_t)source[2] << 8U) |
           (uint32_t)source[3];
}

static uint64_t read_u64(const uint8_t *source)
{
    return ((uint64_t)read_u32(source) << 32U) |
           (uint64_t)read_u32(source + 4U);
}

static bool string_is_bounded(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool write_string(
    uint8_t *target,
    size_t target_size,
    const char *source
)
{
    size_t length;

    if (!string_is_bounded(source, target_size)) {
        return false;
    }
    length = strlen(source);
    memset(target, 0, target_size);
    memcpy(target, source, length);
    return true;
}

static bool read_string(
    char *target,
    size_t target_size,
    const uint8_t *source
)
{
    const uint8_t *terminator;
    size_t length;

    terminator = memchr(source, '\0', target_size);
    if (terminator == NULL) {
        return false;
    }
    length = (size_t)(terminator - source);
    memset(target, 0, target_size);
    memcpy(target, source, length);
    return true;
}

static void write_header(
    uint8_t *frame,
    uint16_t type,
    uint32_t request_id,
    uint32_t payload_length
)
{
    write_u32(frame, AGENT_IPC_MAGIC);
    write_u16(frame + 4U, AGENT_IPC_VERSION);
    write_u16(frame + 6U, type);
    write_u32(frame + 8U, request_id);
    write_u32(frame + 12U, payload_length);
}

enum agent_ipc_result agent_ipc_decode_header(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_header *header
)
{
    if (frame == NULL || header == NULL ||
        frame_length < AGENT_IPC_HEADER_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }
    if (read_u32(frame) != AGENT_IPC_MAGIC) {
        return AGENT_IPC_INVALID;
    }

    header->version = read_u16(frame + 4U);
    header->type = read_u16(frame + 6U);
    header->request_id = read_u32(frame + 8U);
    header->payload_length = read_u32(frame + 12U);

    if ((header->version < AGENT_IPC_MIN_VERSION ||
         header->version > AGENT_IPC_VERSION) ||
        header->payload_length >
            AGENT_IPC_MAX_FRAME_SIZE - AGENT_IPC_HEADER_SIZE ||
        frame_length != AGENT_IPC_HEADER_SIZE +
                        (size_t)header->payload_length) {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_lookup_request(
    const struct agent_ipc_lookup_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;

    if (request == NULL || frame == NULL || frame_length == NULL ||
        request->version == 0U ||
        request->min_trust_level > 100U ||
        request->intent[0] == '\0' ||
        request->tenant[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    if (capacity < AGENT_IPC_HEADER_SIZE + LOOKUP_REQUEST_PAYLOAD_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }

    write_header(frame, AGENT_IPC_LOOKUP_REQUEST, request->request_id,
                 LOOKUP_REQUEST_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u32(payload, request->version);
    write_u64(payload + 4U, request->max_cost_microunits);
    write_u32(payload + 12U, request->max_latency_ms);
    payload[16] = request->min_trust_level;

    if (!write_string(payload + 17U, AGENT_IPC_INTENT_LEN,
                      request->intent) ||
        !write_string(payload + 17U + AGENT_IPC_INTENT_LEN,
                      AGENT_IPC_TENANT_LEN, request->tenant) ||
        !write_string(payload + 17U + AGENT_IPC_INTENT_LEN +
                      AGENT_IPC_TENANT_LEN,
                      AGENT_IPC_REGION_LEN, request->region) ||
        !write_string(payload + 17U + AGENT_IPC_INTENT_LEN +
                      AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN,
                      AGENT_IPC_AGENT_ID_LEN, request->source_agent) ||
        !write_string(payload + 17U + AGENT_IPC_INTENT_LEN +
                      AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN +
                      AGENT_IPC_AGENT_ID_LEN,
                      AGENT_IPC_URI_LEN, request->target_agent) ||
        !write_string(payload + 17U + AGENT_IPC_INTENT_LEN +
                      AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN +
                      AGENT_IPC_AGENT_ID_LEN + AGENT_IPC_URI_LEN,
                      AGENT_IPC_IPV6_LEN, request->public_ipv6)) {
        return AGENT_IPC_INVALID;
    }

    *frame_length = AGENT_IPC_HEADER_SIZE + LOOKUP_REQUEST_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_lookup_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_lookup_request *request
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    enum agent_ipc_result result;

    if (request == NULL) {
        return AGENT_IPC_INVALID;
    }
    result = agent_ipc_decode_header(frame, frame_length, &header);
    if (result != AGENT_IPC_OK) {
        return result;
    }
    if (header.type != AGENT_IPC_LOOKUP_REQUEST ||
        header.payload_length != LOOKUP_REQUEST_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }

    memset(request, 0, sizeof(*request));
    request->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    request->version = read_u32(payload);
    request->max_cost_microunits = read_u64(payload + 4U);
    request->max_latency_ms = read_u32(payload + 12U);
    request->min_trust_level = payload[16];

    if (request->version == 0U ||
        request->min_trust_level > 100U ||
        !read_string(request->intent, sizeof(request->intent),
                     payload + 17U) ||
        !read_string(request->tenant, sizeof(request->tenant),
                     payload + 17U + AGENT_IPC_INTENT_LEN) ||
        !read_string(request->region, sizeof(request->region),
                     payload + 17U + AGENT_IPC_INTENT_LEN +
                     AGENT_IPC_TENANT_LEN) ||
        !read_string(request->source_agent, sizeof(request->source_agent),
                     payload + 17U + AGENT_IPC_INTENT_LEN +
                     AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN) ||
        !read_string(request->target_agent, sizeof(request->target_agent),
                     payload + 17U + AGENT_IPC_INTENT_LEN +
                     AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN +
                     AGENT_IPC_AGENT_ID_LEN) ||
        !read_string(request->public_ipv6, sizeof(request->public_ipv6),
                     payload + 17U + AGENT_IPC_INTENT_LEN +
                     AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN +
                     AGENT_IPC_AGENT_ID_LEN + AGENT_IPC_URI_LEN) ||
        request->intent[0] == '\0' ||
        request->tenant[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_lookup_response(
    const struct agent_ipc_lookup_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;

    if (response == NULL || frame == NULL || frame_length == NULL) {
        return AGENT_IPC_INVALID;
    }
    if (capacity < AGENT_IPC_HEADER_SIZE + LOOKUP_RESPONSE_PAYLOAD_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }

    write_header(frame, AGENT_IPC_LOOKUP_RESPONSE, response->request_id,
                 LOOKUP_RESPONSE_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    payload[0] = response->found ? 1U : 0U;
    write_u64(payload + 1U, response->generation);
    write_u64(payload + 9U, response->score);

    if (!write_string(payload + 17U, AGENT_IPC_ROUTE_ID_LEN,
                      response->route_id) ||
        !write_string(payload + 17U + AGENT_IPC_ROUTE_ID_LEN,
                      AGENT_IPC_URI_LEN, response->origin) ||
        !write_string(payload + 17U + AGENT_IPC_ROUTE_ID_LEN +
                      AGENT_IPC_URI_LEN,
                      AGENT_IPC_URI_LEN, response->endpoint) ||
        !write_string(payload + 17U + AGENT_IPC_ROUTE_ID_LEN +
                      (2U * AGENT_IPC_URI_LEN),
                      AGENT_IPC_SOURCE_LEN, response->source)) {
        return AGENT_IPC_INVALID;
    }
    payload[17U + AGENT_IPC_ROUTE_ID_LEN + (2U * AGENT_IPC_URI_LEN) +
            AGENT_IPC_SOURCE_LEN] = response->relay ? 1U : 0U;
    if (!write_string(
            payload + 18U + AGENT_IPC_ROUTE_ID_LEN +
                (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN,
            AGENT_IPC_PEER_ID_LEN, response->relay_peer_id) ||
        !write_string(
            payload + 18U + AGENT_IPC_ROUTE_ID_LEN +
                (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN +
                AGENT_IPC_PEER_ID_LEN,
            AGENT_IPC_ROUTER_ID_LEN, response->target_router_id)) {
        return AGENT_IPC_INVALID;
    }
    {
        size_t offset = 18U + AGENT_IPC_ROUTE_ID_LEN +
            (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN +
            AGENT_IPC_PEER_ID_LEN + AGENT_IPC_ROUTER_ID_LEN;
        if (!write_string(payload + offset, AGENT_IPC_POLICY_ID_LEN,
                          response->policy_id)) return AGENT_IPC_INVALID;
        write_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN,
                  response->tenant_max_inflight);
        write_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 4U,
                  response->tenant_rate_per_second);
        write_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 8U,
                  response->tenant_rate_burst);
    }

    *frame_length = AGENT_IPC_HEADER_SIZE + LOOKUP_RESPONSE_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_lookup_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_lookup_response *response
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    enum agent_ipc_result result;

    if (response == NULL) {
        return AGENT_IPC_INVALID;
    }
    result = agent_ipc_decode_header(frame, frame_length, &header);
    if (result != AGENT_IPC_OK) {
        return result;
    }
    if (header.type != AGENT_IPC_LOOKUP_RESPONSE ||
        header.payload_length != LOOKUP_RESPONSE_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }

    memset(response, 0, sizeof(*response));
    response->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    if (payload[0] > 1U) {
        return AGENT_IPC_INVALID;
    }
    response->found = payload[0] == 1U;
    response->generation = read_u64(payload + 1U);
    response->score = read_u64(payload + 9U);

    if (!read_string(response->route_id, sizeof(response->route_id),
                     payload + 17U) ||
        !read_string(response->origin, sizeof(response->origin),
                     payload + 17U + AGENT_IPC_ROUTE_ID_LEN) ||
        !read_string(response->endpoint, sizeof(response->endpoint),
                     payload + 17U + AGENT_IPC_ROUTE_ID_LEN +
                     AGENT_IPC_URI_LEN) ||
        !read_string(response->source, sizeof(response->source),
                     payload + 17U + AGENT_IPC_ROUTE_ID_LEN +
                      (2U * AGENT_IPC_URI_LEN))) {
        return AGENT_IPC_INVALID;
    }
    if (payload[17U + AGENT_IPC_ROUTE_ID_LEN + (2U * AGENT_IPC_URI_LEN) +
                AGENT_IPC_SOURCE_LEN] > 1U ||
        !read_string(
            response->relay_peer_id, sizeof(response->relay_peer_id),
            payload + 18U + AGENT_IPC_ROUTE_ID_LEN +
                (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN) ||
        !read_string(
            response->target_router_id, sizeof(response->target_router_id),
            payload + 18U + AGENT_IPC_ROUTE_ID_LEN +
                (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN +
                AGENT_IPC_PEER_ID_LEN)) {
        return AGENT_IPC_INVALID;
    }
    response->relay = payload[17U + AGENT_IPC_ROUTE_ID_LEN +
        (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN] == 1U;
    {
        size_t offset = 18U + AGENT_IPC_ROUTE_ID_LEN +
            (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN +
            AGENT_IPC_PEER_ID_LEN + AGENT_IPC_ROUTER_ID_LEN;
        if (!read_string(response->policy_id, sizeof(response->policy_id),
                         payload + offset)) return AGENT_IPC_INVALID;
        response->tenant_max_inflight =
            read_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN);
        response->tenant_rate_per_second =
            read_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 4U);
        response->tenant_rate_burst =
            read_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 8U);
        if ((response->tenant_rate_per_second == 0U) !=
            (response->tenant_rate_burst == 0U)) return AGENT_IPC_INVALID;
    }
    if (response->found &&
        (response->route_id[0] == '\0' ||
         response->endpoint[0] == '\0' ||
         (response->relay &&
          (response->relay_peer_id[0] == '\0' ||
           response->target_router_id[0] == '\0')))) {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_candidates_request(
    const struct agent_ipc_candidates_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t temporary[AGENT_IPC_MAX_FRAME_SIZE];
    size_t temporary_length;

    if (request == NULL || frame == NULL || frame_length == NULL ||
        request->max_candidates < 2U ||
        request->max_candidates > AGENT_IPC_MAX_CANDIDATES) {
        return AGENT_IPC_INVALID;
    }
    if (capacity < AGENT_IPC_HEADER_SIZE + CANDIDATES_REQUEST_PAYLOAD_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }
    if (agent_ipc_encode_lookup_request(
            &request->lookup, temporary, sizeof(temporary),
            &temporary_length) != AGENT_IPC_OK) {
        return AGENT_IPC_INVALID;
    }
    write_header(frame, AGENT_IPC_CANDIDATES_REQUEST,
                 request->lookup.request_id,
                 CANDIDATES_REQUEST_PAYLOAD_SIZE);
    memcpy(frame + AGENT_IPC_HEADER_SIZE,
           temporary + AGENT_IPC_HEADER_SIZE,
           LOOKUP_REQUEST_PAYLOAD_SIZE);
    frame[AGENT_IPC_HEADER_SIZE + LOOKUP_REQUEST_PAYLOAD_SIZE] =
        request->max_candidates;
    *frame_length = AGENT_IPC_HEADER_SIZE + CANDIDATES_REQUEST_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_candidates_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_candidates_request *request
)
{
    struct agent_ipc_header header;
    uint8_t temporary[AGENT_IPC_MAX_FRAME_SIZE];
    size_t lookup_frame_length =
        AGENT_IPC_HEADER_SIZE + LOOKUP_REQUEST_PAYLOAD_SIZE;
    enum agent_ipc_result result;

    if (request == NULL) return AGENT_IPC_INVALID;
    result = agent_ipc_decode_header(frame, frame_length, &header);
    if (result != AGENT_IPC_OK) return result;
    if (header.type != AGENT_IPC_CANDIDATES_REQUEST ||
        header.payload_length != CANDIDATES_REQUEST_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    memcpy(temporary, frame, lookup_frame_length);
    write_header(temporary, AGENT_IPC_LOOKUP_REQUEST, header.request_id,
                 LOOKUP_REQUEST_PAYLOAD_SIZE);
    memset(request, 0, sizeof(*request));
    result = agent_ipc_decode_lookup_request(
        temporary, lookup_frame_length, &request->lookup);
    if (result != AGENT_IPC_OK) return result;
    request->max_candidates =
        frame[AGENT_IPC_HEADER_SIZE + LOOKUP_REQUEST_PAYLOAD_SIZE];
    if (request->max_candidates < 2U ||
        request->max_candidates > AGENT_IPC_MAX_CANDIDATES) {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

static bool write_candidate(
    uint8_t *target,
    const struct agent_ipc_route_candidate *candidate
)
{
    write_u64(target, candidate->score);
    return write_string(target + 8U, AGENT_IPC_ROUTE_ID_LEN,
                        candidate->route_id) &&
           write_string(target + 8U + AGENT_IPC_ROUTE_ID_LEN,
                        AGENT_IPC_URI_LEN, candidate->origin) &&
           write_string(target + 8U + AGENT_IPC_ROUTE_ID_LEN +
                        AGENT_IPC_URI_LEN,
                        AGENT_IPC_URI_LEN, candidate->endpoint) &&
           write_string(target + 8U + AGENT_IPC_ROUTE_ID_LEN +
                        (2U * AGENT_IPC_URI_LEN),
                        AGENT_IPC_SOURCE_LEN, candidate->source) &&
           ((target[8U + AGENT_IPC_ROUTE_ID_LEN +
                    (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN] =
                 candidate->relay ? 1U : 0U), true) &&
           write_string(target + 9U + AGENT_IPC_ROUTE_ID_LEN +
                        (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN,
                        AGENT_IPC_PEER_ID_LEN,
                        candidate->relay_peer_id) &&
           write_string(target + 9U + AGENT_IPC_ROUTE_ID_LEN +
                        (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN +
                        AGENT_IPC_PEER_ID_LEN,
                        AGENT_IPC_ROUTER_ID_LEN,
                        candidate->target_router_id);
}

static bool read_candidate(
    struct agent_ipc_route_candidate *candidate,
    const uint8_t *source
)
{
    memset(candidate, 0, sizeof(*candidate));
    candidate->score = read_u64(source);
    return read_string(candidate->route_id, sizeof(candidate->route_id),
                       source + 8U) &&
           read_string(candidate->origin, sizeof(candidate->origin),
                       source + 8U + AGENT_IPC_ROUTE_ID_LEN) &&
           read_string(candidate->endpoint, sizeof(candidate->endpoint),
                       source + 8U + AGENT_IPC_ROUTE_ID_LEN +
                       AGENT_IPC_URI_LEN) &&
           read_string(candidate->source, sizeof(candidate->source),
                       source + 8U + AGENT_IPC_ROUTE_ID_LEN +
                       (2U * AGENT_IPC_URI_LEN)) &&
           source[8U + AGENT_IPC_ROUTE_ID_LEN +
                  (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN] <= 1U &&
           ((candidate->relay = source[8U + AGENT_IPC_ROUTE_ID_LEN +
               (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN] == 1U),
            true) &&
           read_string(candidate->relay_peer_id,
                       sizeof(candidate->relay_peer_id),
                       source + 9U + AGENT_IPC_ROUTE_ID_LEN +
                       (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN) &&
           read_string(candidate->target_router_id,
                       sizeof(candidate->target_router_id),
                       source + 9U + AGENT_IPC_ROUTE_ID_LEN +
                       (2U * AGENT_IPC_URI_LEN) + AGENT_IPC_SOURCE_LEN +
                       AGENT_IPC_PEER_ID_LEN);
}

enum agent_ipc_result agent_ipc_encode_candidates_response(
    const struct agent_ipc_candidates_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;
    size_t index;

    if (response == NULL || frame == NULL || frame_length == NULL ||
        response->count > AGENT_IPC_MAX_CANDIDATES) {
        return AGENT_IPC_INVALID;
    }
    if (capacity < AGENT_IPC_HEADER_SIZE + CANDIDATES_RESPONSE_PAYLOAD_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }
    write_header(frame, AGENT_IPC_CANDIDATES_RESPONSE,
                 response->request_id, CANDIDATES_RESPONSE_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    memset(payload, 0, CANDIDATES_RESPONSE_PAYLOAD_SIZE);
    payload[0] = response->count;
    write_u64(payload + 1U, response->generation);
    for (index = 0U; index < response->count; index++) {
        if (response->candidates[index].route_id[0] == '\0' ||
            response->candidates[index].endpoint[0] == '\0' ||
            !write_candidate(payload + 9U + index * CANDIDATE_ITEM_SIZE,
                             &response->candidates[index])) {
            return AGENT_IPC_INVALID;
        }
    }
    {
        size_t offset = 9U + AGENT_IPC_MAX_CANDIDATES * CANDIDATE_ITEM_SIZE;
        if (!write_string(payload + offset, AGENT_IPC_POLICY_ID_LEN,
                          response->policy_id)) return AGENT_IPC_INVALID;
        write_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN,
                  response->tenant_max_inflight);
        write_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 4U,
                  response->tenant_rate_per_second);
        write_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 8U,
                  response->tenant_rate_burst);
    }
    *frame_length = AGENT_IPC_HEADER_SIZE + CANDIDATES_RESPONSE_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_candidates_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_candidates_response *response
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    size_t index;
    enum agent_ipc_result result;

    if (response == NULL) return AGENT_IPC_INVALID;
    result = agent_ipc_decode_header(frame, frame_length, &header);
    if (result != AGENT_IPC_OK) return result;
    if (header.type != AGENT_IPC_CANDIDATES_RESPONSE ||
        header.payload_length != CANDIDATES_RESPONSE_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    memset(response, 0, sizeof(*response));
    response->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    response->count = payload[0];
    response->generation = read_u64(payload + 1U);
    if (response->count > AGENT_IPC_MAX_CANDIDATES) {
        return AGENT_IPC_INVALID;
    }
    for (index = 0U; index < response->count; index++) {
        if (!read_candidate(
                &response->candidates[index],
                payload + 9U + index * CANDIDATE_ITEM_SIZE) ||
            response->candidates[index].route_id[0] == '\0' ||
            response->candidates[index].endpoint[0] == '\0') {
            return AGENT_IPC_INVALID;
        }
        if (index > 0U &&
            strcmp(response->candidates[index - 1U].route_id,
                   response->candidates[index].route_id) == 0) {
            return AGENT_IPC_INVALID;
        }
    }
    {
        size_t offset = 9U + AGENT_IPC_MAX_CANDIDATES * CANDIDATE_ITEM_SIZE;
        if (!read_string(response->policy_id, sizeof(response->policy_id),
                         payload + offset)) return AGENT_IPC_INVALID;
        response->tenant_max_inflight =
            read_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN);
        response->tenant_rate_per_second =
            read_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 4U);
        response->tenant_rate_burst =
            read_u32(payload + offset + AGENT_IPC_POLICY_ID_LEN + 8U);
        if ((response->tenant_rate_per_second == 0U) !=
            (response->tenant_rate_burst == 0U)) return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_error_response(
    const struct agent_ipc_error_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;

    if (response == NULL || frame == NULL || frame_length == NULL ||
        response->message[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    if (capacity < AGENT_IPC_HEADER_SIZE + ERROR_RESPONSE_PAYLOAD_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }

    write_header(frame, AGENT_IPC_ERROR_RESPONSE, response->request_id,
                 ERROR_RESPONSE_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u32(payload, response->code);
    if (!write_string(payload + 4U, AGENT_IPC_ERROR_LEN,
                      response->message)) {
        return AGENT_IPC_INVALID;
    }
    *frame_length = AGENT_IPC_HEADER_SIZE + ERROR_RESPONSE_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_error_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_error_response *response
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    enum agent_ipc_result result;

    if (response == NULL) {
        return AGENT_IPC_INVALID;
    }
    result = agent_ipc_decode_header(frame, frame_length, &header);
    if (result != AGENT_IPC_OK) {
        return result;
    }
    if (header.type != AGENT_IPC_ERROR_RESPONSE ||
        header.payload_length != ERROR_RESPONSE_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }

    memset(response, 0, sizeof(*response));
    response->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    response->code = read_u32(payload);
    if (!read_string(response->message, sizeof(response->message),
                     payload + 4U) ||
        response->message[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

static enum agent_ipc_result encode_invoke_request_type(
    const struct agent_ipc_invoke_request *request,
    enum agent_ipc_message_type type,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;
    size_t offset = 0U;
    size_t payload_length;

    if (request == NULL || frame == NULL || frame_length == NULL ||
        request->request_id == 0U || request->timeout_ms == 0U ||
        request->hop_limit == 0U || request->body_length == 0U ||
        request->body_length > AGENT_IPC_MAX_INVOKE_BODY ||
        request->route_id[0] == '\0' || request->relay_peer_id[0] == '\0' ||
        request->target_router_id[0] == '\0' || request->intent[0] == '\0' ||
        request->task_id[0] == '\0' || request->source_agent[0] == '\0' ||
        request->tenant[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    payload_length = INVOKE_REQUEST_FIXED_PAYLOAD_SIZE + request->body_length;
    if (payload_length > UINT32_MAX ||
        capacity < AGENT_IPC_HEADER_SIZE + payload_length) {
        return AGENT_IPC_TOO_SMALL;
    }
    write_header(frame, (uint16_t)type, request->request_id,
                 (uint32_t)payload_length);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u32(payload + offset, request->timeout_ms); offset += 4U;
    payload[offset++] = request->hop_limit;
    write_u64(payload + offset, request->max_cost_microunits); offset += 8U;
    write_u32(payload + offset, request->max_latency_ms); offset += 4U;
#define WRITE_INVOKE_FIELD(field, length) \
    do { \
        if (!write_string(payload + offset, (length), request->field)) \
            return AGENT_IPC_INVALID; \
        offset += (length); \
    } while (0)
    WRITE_INVOKE_FIELD(route_id, AGENT_IPC_ROUTE_ID_LEN);
    WRITE_INVOKE_FIELD(relay_peer_id, AGENT_IPC_PEER_ID_LEN);
    WRITE_INVOKE_FIELD(target_router_id, AGENT_IPC_ROUTER_ID_LEN);
    WRITE_INVOKE_FIELD(intent, AGENT_IPC_INTENT_LEN);
    WRITE_INVOKE_FIELD(task_id, AGENT_IPC_TASK_ID_LEN);
    WRITE_INVOKE_FIELD(source_agent, AGENT_IPC_AGENT_ID_LEN);
    WRITE_INVOKE_FIELD(target_agent, AGENT_IPC_URI_LEN);
    WRITE_INVOKE_FIELD(tenant, AGENT_IPC_TENANT_LEN);
    WRITE_INVOKE_FIELD(region, AGENT_IPC_REGION_LEN);
    WRITE_INVOKE_FIELD(forwarding_assertion,
                       AGENT_IPC_FORWARDING_ASSERTION_LEN);
#undef WRITE_INVOKE_FIELD
    write_u32(payload + offset, (uint32_t)request->body_length); offset += 4U;
    memcpy(payload + offset, request->body, request->body_length);
    *frame_length = AGENT_IPC_HEADER_SIZE + payload_length;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_invoke_request(
    const struct agent_ipc_invoke_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    return encode_invoke_request_type(
        request, AGENT_IPC_INVOKE_REQUEST,
        frame, capacity, frame_length);
}

enum agent_ipc_result agent_ipc_encode_stream_request(
    const struct agent_ipc_invoke_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    return encode_invoke_request_type(
        request, AGENT_IPC_STREAM_REQUEST,
        frame, capacity, frame_length);
}

static enum agent_ipc_result decode_invoke_request_type(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_request *request,
    enum agent_ipc_message_type type
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    size_t offset = 0U;
    uint32_t body_length;

    if (request == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != (uint16_t)type ||
        header.payload_length < INVOKE_REQUEST_FIXED_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    memset(request, 0, sizeof(*request));
    request->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    request->timeout_ms = read_u32(payload + offset); offset += 4U;
    request->hop_limit = payload[offset++];
    request->max_cost_microunits = read_u64(payload + offset); offset += 8U;
    request->max_latency_ms = read_u32(payload + offset); offset += 4U;
#define READ_INVOKE_FIELD(field, length) \
    do { \
        if (!read_string(request->field, sizeof(request->field), \
                         payload + offset)) return AGENT_IPC_INVALID; \
        offset += (length); \
    } while (0)
    READ_INVOKE_FIELD(route_id, AGENT_IPC_ROUTE_ID_LEN);
    READ_INVOKE_FIELD(relay_peer_id, AGENT_IPC_PEER_ID_LEN);
    READ_INVOKE_FIELD(target_router_id, AGENT_IPC_ROUTER_ID_LEN);
    READ_INVOKE_FIELD(intent, AGENT_IPC_INTENT_LEN);
    READ_INVOKE_FIELD(task_id, AGENT_IPC_TASK_ID_LEN);
    READ_INVOKE_FIELD(source_agent, AGENT_IPC_AGENT_ID_LEN);
    READ_INVOKE_FIELD(target_agent, AGENT_IPC_URI_LEN);
    READ_INVOKE_FIELD(tenant, AGENT_IPC_TENANT_LEN);
    READ_INVOKE_FIELD(region, AGENT_IPC_REGION_LEN);
    READ_INVOKE_FIELD(forwarding_assertion,
                      AGENT_IPC_FORWARDING_ASSERTION_LEN);
#undef READ_INVOKE_FIELD
    body_length = read_u32(payload + offset); offset += 4U;
    if (request->request_id == 0U || request->timeout_ms == 0U ||
        request->hop_limit == 0U || body_length == 0U ||
        body_length > AGENT_IPC_MAX_INVOKE_BODY ||
        header.payload_length != INVOKE_REQUEST_FIXED_PAYLOAD_SIZE +
                                 body_length ||
        request->route_id[0] == '\0' || request->relay_peer_id[0] == '\0' ||
        request->target_router_id[0] == '\0' || request->intent[0] == '\0' ||
        request->task_id[0] == '\0' || request->source_agent[0] == '\0' ||
        request->tenant[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    request->body_length = body_length;
    memcpy(request->body, payload + offset, body_length);
    request->streaming = type == AGENT_IPC_STREAM_REQUEST;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_invoke_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_request *request
)
{
    return decode_invoke_request_type(
        frame, frame_length, request, AGENT_IPC_INVOKE_REQUEST);
}

enum agent_ipc_result agent_ipc_decode_stream_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_request *request
)
{
    return decode_invoke_request_type(
        frame, frame_length, request, AGENT_IPC_STREAM_REQUEST);
}

enum agent_ipc_result agent_ipc_encode_invoke_response(
    const struct agent_ipc_invoke_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;
    size_t payload_length;

    if (response == NULL || frame == NULL || frame_length == NULL ||
        response->request_id == 0U || response->status_code < 100U ||
        response->status_code > 599U || response->content_type[0] == '\0' ||
        response->body_length > AGENT_IPC_MAX_INVOKE_BODY) {
        return AGENT_IPC_INVALID;
    }
    payload_length = INVOKE_RESPONSE_FIXED_PAYLOAD_SIZE + response->body_length;
    if (capacity < AGENT_IPC_HEADER_SIZE + payload_length) {
        return AGENT_IPC_TOO_SMALL;
    }
    write_header(frame, AGENT_IPC_INVOKE_RESPONSE, response->request_id,
                 (uint32_t)payload_length);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u16(payload, response->status_code);
    if (!write_string(payload + 2U, AGENT_IPC_CONTENT_TYPE_LEN,
                      response->content_type)) {
        return AGENT_IPC_INVALID;
    }
    write_u32(payload + 2U + AGENT_IPC_CONTENT_TYPE_LEN,
              (uint32_t)response->body_length);
    memcpy(payload + INVOKE_RESPONSE_FIXED_PAYLOAD_SIZE,
           response->body, response->body_length);
    *frame_length = AGENT_IPC_HEADER_SIZE + payload_length;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_invoke_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_invoke_response *response
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    uint32_t body_length;

    if (response == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_INVOKE_RESPONSE ||
        header.payload_length < INVOKE_RESPONSE_FIXED_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    payload = frame + AGENT_IPC_HEADER_SIZE;
    body_length = read_u32(payload + 2U + AGENT_IPC_CONTENT_TYPE_LEN);
    if (body_length > AGENT_IPC_MAX_INVOKE_BODY ||
        header.payload_length != INVOKE_RESPONSE_FIXED_PAYLOAD_SIZE +
                                 body_length) {
        return AGENT_IPC_INVALID;
    }
    memset(response, 0, sizeof(*response));
    response->request_id = header.request_id;
    response->status_code = read_u16(payload);
    if (response->request_id == 0U || response->status_code < 100U ||
        response->status_code > 599U ||
        !read_string(response->content_type, sizeof(response->content_type),
                     payload + 2U) || response->content_type[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    response->body_length = body_length;
    memcpy(response->body, payload + INVOKE_RESPONSE_FIXED_PAYLOAD_SIZE,
           body_length);
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_stream_start(
    const struct agent_ipc_stream_start *start,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;

    if (start == NULL || frame == NULL || frame_length == NULL ||
        start->request_id == 0U || start->status_code != 200U ||
        start->content_type[0] == '\0' ||
        capacity < AGENT_IPC_HEADER_SIZE + STREAM_START_PAYLOAD_SIZE) {
        return AGENT_IPC_INVALID;
    }
    write_header(frame, AGENT_IPC_STREAM_START, start->request_id,
                 STREAM_START_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u16(payload, start->status_code);
    if (!write_string(payload + 2U, AGENT_IPC_CONTENT_TYPE_LEN,
                      start->content_type)) return AGENT_IPC_INVALID;
    *frame_length = AGENT_IPC_HEADER_SIZE + STREAM_START_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_stream_start(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_stream_start *start
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;

    if (start == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_STREAM_START ||
        header.payload_length != STREAM_START_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    memset(start, 0, sizeof(*start));
    start->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    start->status_code = read_u16(payload);
    if (start->request_id == 0U || start->status_code != 200U ||
        !read_string(start->content_type, sizeof(start->content_type),
                     payload + 2U) || start->content_type[0] == '\0') {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_stream_data(
    const struct agent_ipc_stream_data *data,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    size_t payload_length;
    uint8_t *payload;

    if (data == NULL || frame == NULL || frame_length == NULL ||
        data->request_id == 0U || data->data_length == 0U ||
        data->data_length > AGENT_IPC_MAX_STREAM_DATA) {
        return AGENT_IPC_INVALID;
    }
    payload_length = STREAM_DATA_FIXED_PAYLOAD_SIZE + data->data_length;
    if (capacity < AGENT_IPC_HEADER_SIZE + payload_length) {
        return AGENT_IPC_TOO_SMALL;
    }
    write_header(frame, AGENT_IPC_STREAM_DATA, data->request_id,
                 (uint32_t)payload_length);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u32(payload, (uint32_t)data->data_length);
    memcpy(payload + STREAM_DATA_FIXED_PAYLOAD_SIZE,
           data->data, data->data_length);
    *frame_length = AGENT_IPC_HEADER_SIZE + payload_length;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_stream_data(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_stream_data *data
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    uint32_t data_length;

    if (data == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_STREAM_DATA ||
        header.payload_length < STREAM_DATA_FIXED_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    payload = frame + AGENT_IPC_HEADER_SIZE;
    data_length = read_u32(payload);
    if (data_length == 0U || data_length > AGENT_IPC_MAX_STREAM_DATA ||
        header.payload_length != STREAM_DATA_FIXED_PAYLOAD_SIZE + data_length) {
        return AGENT_IPC_INVALID;
    }
    memset(data, 0, sizeof(*data));
    data->request_id = header.request_id;
    data->data_length = data_length;
    memcpy(data->data, payload + STREAM_DATA_FIXED_PAYLOAD_SIZE, data_length);
    return data->request_id == 0U ? AGENT_IPC_INVALID : AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_stream_end(
    const struct agent_ipc_stream_end *end,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    if (end == NULL || frame == NULL || frame_length == NULL ||
        end->request_id == 0U || end->total_bytes > AGENT_IPC_MAX_STREAM_BYTES ||
        capacity < AGENT_IPC_HEADER_SIZE + STREAM_END_PAYLOAD_SIZE) {
        return AGENT_IPC_INVALID;
    }
    write_header(frame, AGENT_IPC_STREAM_END, end->request_id,
                 STREAM_END_PAYLOAD_SIZE);
    write_u64(frame + AGENT_IPC_HEADER_SIZE, end->total_bytes);
    *frame_length = AGENT_IPC_HEADER_SIZE + STREAM_END_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_stream_end(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_stream_end *end
)
{
    struct agent_ipc_header header;

    if (end == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_STREAM_END ||
        header.payload_length != STREAM_END_PAYLOAD_SIZE) {
        return AGENT_IPC_WRONG_TYPE;
    }
    end->request_id = header.request_id;
    end->total_bytes = read_u64(frame + AGENT_IPC_HEADER_SIZE);
    if (end->request_id == 0U || end->total_bytes > AGENT_IPC_MAX_STREAM_BYTES) {
        return AGENT_IPC_INVALID;
    }
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_register_request(
    const struct agent_ipc_register_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;
    size_t offset = 25U;

    if (request == NULL || frame == NULL || frame_length == NULL ||
        request->request_id == 0U || request->version == 0U ||
        request->intent[0] == '\0' || request->origin[0] == '\0' ||
        request->endpoint[0] == '\0' || request->tenant[0] == '\0' ||
        request->trust_level > 100U || request->load_permille > 1000U ||
        (request->cloud.present &&
         (request->cloud.agent_name[0] == '\0' ||
          request->cloud.manifest_digest[0] == '\0')) ||
        (request->cloud.tool_present &&
         (!request->cloud.present || request->cloud.tool_name[0] == '\0' ||
          ((request->cloud.resumable || request->cloud.continuable) &&
           !request->cloud.task) ||
          request->cloud.recovery_protocol > 1U ||
          (request->cloud.chat &&
           (!request->cloud.task || !request->cloud.interactive)))) ||
        (request->cloud.computer_present &&
         (request->cloud.computer_requirement > AGENT_IPC_COMPUTER_REQUIRED ||
          (request->cloud.workspace_capabilities &
           (uint16_t)~AGENT_IPC_WORKSPACE_ALL) != 0U ||
          (request->cloud.computer_requirement == AGENT_IPC_COMPUTER_DISABLED &&
           request->cloud.workspace_capabilities != 0U))) ||
        (!request->cloud.computer_present &&
         (request->cloud.computer_requirement != AGENT_IPC_COMPUTER_DISABLED ||
          request->cloud.workspace_capabilities != 0U)) ||
        (request->cloud.mobile_present &&
         (request->cloud.mobile_requirement > AGENT_IPC_MOBILE_REQUIRED ||
          (request->cloud.mobile_capabilities &
           (uint16_t)~AGENT_IPC_MOBILE_ALL) != 0U ||
          (request->cloud.mobile_scopes &
           (uint16_t)~AGENT_IPC_MOBILE_ALL) != 0U ||
          (request->cloud.mobile_requirement == AGENT_IPC_MOBILE_DISABLED &&
           request->cloud.mobile_capabilities != 0U) ||
          (request->cloud.mobile_requirement == AGENT_IPC_MOBILE_REQUIRED &&
           request->cloud.mobile_capabilities == 0U) ||
          (request->cloud.mobile_scopes &
           (uint16_t)~request->cloud.mobile_capabilities) != 0U)) ||
        (!request->cloud.mobile_present &&
         (request->cloud.mobile_requirement != AGENT_IPC_MOBILE_DISABLED ||
          request->cloud.mobile_capabilities != 0U ||
          request->cloud.mobile_scopes != 0U))) {
        return AGENT_IPC_INVALID;
    }
    if (capacity < AGENT_IPC_HEADER_SIZE + REGISTER_REQUEST_PAYLOAD_SIZE) {
        return AGENT_IPC_TOO_SMALL;
    }
    write_header(frame, AGENT_IPC_REGISTER_REQUEST, request->request_id,
                 REGISTER_REQUEST_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u32(payload, request->version);
    write_u32(payload + 4U, request->lease_seconds);
    write_u64(payload + 8U, request->cost_microunits);
    write_u32(payload + 16U, request->latency_ms);
    payload[20] = request->trust_level;
    write_u16(payload + 21U, request->load_permille);
    payload[23] = request->hop_count;
    payload[24] = request->request_public_ipv6 ? 1U : 0U;
    if (!write_string(payload + offset, AGENT_IPC_ROUTE_ID_LEN,
                      request->route_id)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_ROUTE_ID_LEN;
    if (!write_string(payload + offset, AGENT_IPC_INTENT_LEN,
                      request->intent)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_INTENT_LEN;
    if (!write_string(payload + offset, AGENT_IPC_URI_LEN,
                      request->origin)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_URI_LEN;
    if (!write_string(payload + offset, AGENT_IPC_URI_LEN,
                      request->endpoint)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_URI_LEN;
    if (!write_string(payload + offset, AGENT_IPC_TENANT_LEN,
                      request->tenant)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TENANT_LEN;
    if (!write_string(payload + offset, AGENT_IPC_REGION_LEN,
                      request->region)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_REGION_LEN;
    payload[offset++] = request->cloud.present ? 1U : 0U;
    payload[offset++] = request->cloud.publish ? 1U : 0U;
    payload[offset++] = request->cloud.tool_present ? 1U : 0U;
    if (!write_string(payload + offset, AGENT_IPC_CLOUD_NAME_LEN,
                      request->cloud.agent_name)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_CLOUD_NAME_LEN;
    if (!write_string(payload + offset, AGENT_IPC_MANIFEST_DIGEST_LEN,
                      request->cloud.manifest_digest)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_MANIFEST_DIGEST_LEN;
    if (!write_string(payload + offset, AGENT_IPC_TOOL_NAME_LEN,
                      request->cloud.tool_name)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_NAME_LEN;
    if (!write_string(payload + offset, AGENT_IPC_TOOL_TITLE_LEN,
                      request->cloud.tool_title)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_TITLE_LEN;
    if (!write_string(payload + offset, AGENT_IPC_TOOL_DESCRIPTION_LEN,
                      request->cloud.tool_description)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_DESCRIPTION_LEN;
    if (!write_string(payload + offset, AGENT_IPC_TOOL_INPUT_SCHEMA_LEN,
                      request->cloud.tool_input_schema)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_INPUT_SCHEMA_LEN;
    payload[offset++] = request->cloud.task ? 1U : 0U;
    payload[offset++] = request->cloud.resumable ? 1U : 0U;
    payload[offset++] = request->cloud.demo ? 1U : 0U;
    payload[offset++] = request->cloud.chat ? 1U : 0U;
    payload[offset++] = request->cloud.interactive ? 1U : 0U;
    payload[offset++] = request->cloud.computer_present ? 1U : 0U;
    payload[offset++] = request->cloud.computer_requirement;
    write_u16(payload + offset, request->cloud.workspace_capabilities);
    offset += 2U;
    payload[offset++] = request->cloud.mobile_present ? 1U : 0U;
    payload[offset++] = request->cloud.mobile_requirement;
    write_u16(payload + offset, request->cloud.mobile_capabilities);
    offset += 2U;
    write_u16(payload + offset, request->cloud.mobile_scopes);
    offset += 2U;
    payload[offset++] = request->cloud.continuable ? 1U : 0U;
    payload[offset] = request->cloud.recovery_protocol;
    *frame_length = AGENT_IPC_HEADER_SIZE + REGISTER_REQUEST_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_register_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_register_request *request
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    size_t offset = 25U;

    if (request == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_REGISTER_REQUEST ||
        (header.version == 8U &&
         header.payload_length != REGISTER_REQUEST_V8_PAYLOAD_SIZE) ||
        (header.version == 9U &&
         header.payload_length != REGISTER_REQUEST_V9_PAYLOAD_SIZE) ||
        (header.version == 10U &&
         header.payload_length != REGISTER_REQUEST_V10_PAYLOAD_SIZE) ||
        (header.version == 11U &&
         header.payload_length != REGISTER_REQUEST_V12_PAYLOAD_SIZE) ||
        (header.version == 12U &&
         header.payload_length != REGISTER_REQUEST_V12_PAYLOAD_SIZE) ||
        (header.version == AGENT_IPC_VERSION &&
         header.payload_length != REGISTER_REQUEST_PAYLOAD_SIZE)) {
        return AGENT_IPC_WRONG_TYPE;
    }
    memset(request, 0, sizeof(*request));
    request->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    request->version = read_u32(payload);
    request->lease_seconds = read_u32(payload + 4U);
    request->cost_microunits = read_u64(payload + 8U);
    request->latency_ms = read_u32(payload + 16U);
    request->trust_level = payload[20];
    request->load_permille = read_u16(payload + 21U);
    request->hop_count = payload[23];
    request->request_public_ipv6 = payload[24] != 0U;
    if (!read_string(request->route_id, sizeof(request->route_id),
                     payload + offset)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_ROUTE_ID_LEN;
    if (!read_string(request->intent, sizeof(request->intent),
                     payload + offset)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_INTENT_LEN;
    if (!read_string(request->origin, sizeof(request->origin),
                     payload + offset)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_URI_LEN;
    if (!read_string(request->endpoint, sizeof(request->endpoint),
                     payload + offset)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_URI_LEN;
    if (!read_string(request->tenant, sizeof(request->tenant),
                     payload + offset)) return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TENANT_LEN;
    if (!read_string(request->region, sizeof(request->region),
                     payload + offset) || request->request_id == 0U ||
        request->version == 0U || request->intent[0] == '\0' ||
        request->origin[0] == '\0' || request->endpoint[0] == '\0' ||
        request->tenant[0] == '\0' || payload[24] > 1U ||
        request->trust_level > 100U ||
        request->load_permille > 1000U) return AGENT_IPC_INVALID;
    if (header.version == 8U) return AGENT_IPC_OK;
    offset += AGENT_IPC_REGION_LEN;
    if (payload[offset] > 1U || payload[offset + 1U] > 1U ||
        payload[offset + 2U] > 1U) return AGENT_IPC_INVALID;
    request->cloud.present = payload[offset++] != 0U;
    request->cloud.publish = payload[offset++] != 0U;
    request->cloud.tool_present = payload[offset++] != 0U;
    if (!read_string(request->cloud.agent_name,
                     sizeof(request->cloud.agent_name), payload + offset))
        return AGENT_IPC_INVALID;
    offset += AGENT_IPC_CLOUD_NAME_LEN;
    if (!read_string(request->cloud.manifest_digest,
                     sizeof(request->cloud.manifest_digest), payload + offset))
        return AGENT_IPC_INVALID;
    offset += AGENT_IPC_MANIFEST_DIGEST_LEN;
    if (!read_string(request->cloud.tool_name,
                     sizeof(request->cloud.tool_name), payload + offset))
        return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_NAME_LEN;
    if (!read_string(request->cloud.tool_title,
                     sizeof(request->cloud.tool_title), payload + offset))
        return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_TITLE_LEN;
    if (!read_string(request->cloud.tool_description,
                     sizeof(request->cloud.tool_description), payload + offset))
        return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_DESCRIPTION_LEN;
    if (!read_string(request->cloud.tool_input_schema,
                     sizeof(request->cloud.tool_input_schema), payload + offset))
        return AGENT_IPC_INVALID;
    offset += AGENT_IPC_TOOL_INPUT_SCHEMA_LEN;
    if (payload[offset] > 1U || payload[offset + 1U] > 1U ||
        payload[offset + 2U] > 1U || payload[offset + 3U] > 1U ||
        payload[offset + 4U] > 1U) return AGENT_IPC_INVALID;
    request->cloud.task = payload[offset++] != 0U;
    request->cloud.resumable = payload[offset++] != 0U;
    request->cloud.demo = payload[offset++] != 0U;
    request->cloud.chat = payload[offset++] != 0U;
    request->cloud.interactive = payload[offset++] != 0U;
    if (header.version >= 10U) {
        if (payload[offset] > 1U) return AGENT_IPC_INVALID;
        request->cloud.computer_present = payload[offset++] != 0U;
        request->cloud.computer_requirement = payload[offset++];
        request->cloud.workspace_capabilities = read_u16(payload + offset);
        offset += 2U;
        if (header.version < 12U &&
            (request->cloud.workspace_capabilities &
             AGENT_IPC_WORKSPACE_BROWSER_CONTROL) != 0U)
            return AGENT_IPC_INVALID;
    }
    if (header.version >= 11U) {
        if (payload[offset] > 1U) return AGENT_IPC_INVALID;
        request->cloud.mobile_present = payload[offset++] != 0U;
        request->cloud.mobile_requirement = payload[offset++];
        request->cloud.mobile_capabilities = read_u16(payload + offset);
        offset += 2U;
        request->cloud.mobile_scopes = read_u16(payload + offset);
        offset += 2U;
    }
    if (header.version >= 13U) {
        if (payload[offset] > 1U || payload[offset + 1U] > 1U)
            return AGENT_IPC_INVALID;
        request->cloud.continuable = payload[offset++] != 0U;
        request->cloud.recovery_protocol = payload[offset];
    } else {
        request->cloud.continuable = request->cloud.resumable;
        request->cloud.recovery_protocol = 0U;
    }
    if ((request->cloud.present &&
         (request->cloud.agent_name[0] == '\0' ||
          request->cloud.manifest_digest[0] == '\0')) ||
        (request->cloud.tool_present &&
         (!request->cloud.present || request->cloud.tool_name[0] == '\0' ||
          ((request->cloud.resumable || request->cloud.continuable) &&
           !request->cloud.task) ||
          request->cloud.recovery_protocol > 1U ||
          (request->cloud.chat &&
           (!request->cloud.task || !request->cloud.interactive)))) ||
        (request->cloud.computer_present &&
         (request->cloud.computer_requirement > AGENT_IPC_COMPUTER_REQUIRED ||
          (request->cloud.workspace_capabilities &
           (uint16_t)~AGENT_IPC_WORKSPACE_ALL) != 0U ||
          (request->cloud.computer_requirement == AGENT_IPC_COMPUTER_DISABLED &&
           request->cloud.workspace_capabilities != 0U))) ||
        (!request->cloud.computer_present &&
         (request->cloud.computer_requirement != AGENT_IPC_COMPUTER_DISABLED ||
          request->cloud.workspace_capabilities != 0U)) ||
        (request->cloud.mobile_present &&
         (request->cloud.mobile_requirement > AGENT_IPC_MOBILE_REQUIRED ||
          (request->cloud.mobile_capabilities &
           (uint16_t)~AGENT_IPC_MOBILE_ALL) != 0U ||
          (request->cloud.mobile_scopes &
           (uint16_t)~AGENT_IPC_MOBILE_ALL) != 0U ||
          (request->cloud.mobile_requirement == AGENT_IPC_MOBILE_DISABLED &&
           request->cloud.mobile_capabilities != 0U) ||
          (request->cloud.mobile_requirement == AGENT_IPC_MOBILE_REQUIRED &&
           request->cloud.mobile_capabilities == 0U) ||
          (request->cloud.mobile_scopes &
           (uint16_t)~request->cloud.mobile_capabilities) != 0U)) ||
        (!request->cloud.mobile_present &&
         (request->cloud.mobile_requirement != AGENT_IPC_MOBILE_DISABLED ||
          request->cloud.mobile_capabilities != 0U ||
          request->cloud.mobile_scopes != 0U)))
        return AGENT_IPC_INVALID;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_renew_request(
    const struct agent_ipc_renew_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;
    uint8_t valid_flags = AGENT_IPC_RENEW_LATENCY |
                          AGENT_IPC_RENEW_LOAD | AGENT_IPC_RENEW_HEALTH;

    if (request == NULL || frame == NULL || frame_length == NULL ||
        request->request_id == 0U || request->route_id[0] == '\0' ||
        (request->update_flags & (uint8_t)~valid_flags) != 0U ||
        ((request->update_flags & AGENT_IPC_RENEW_LOAD) != 0U &&
         request->load_permille > 1000U)) return AGENT_IPC_INVALID;
    if (capacity < AGENT_IPC_HEADER_SIZE + RENEW_REQUEST_PAYLOAD_SIZE)
        return AGENT_IPC_TOO_SMALL;
    write_header(frame, AGENT_IPC_RENEW_REQUEST, request->request_id,
                 RENEW_REQUEST_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u32(payload, request->lease_seconds);
    write_u32(payload + 4U, request->latency_ms);
    write_u16(payload + 8U, request->load_permille);
    payload[10] = request->update_flags;
    payload[11] = request->healthy ? 1U : 0U;
    if (!write_string(payload + 12U, AGENT_IPC_ROUTE_ID_LEN,
                      request->route_id)) return AGENT_IPC_INVALID;
    if (!write_string(payload + 12U + AGENT_IPC_ROUTE_ID_LEN,
                      AGENT_IPC_AGENT_ID_LEN, request->requester_agent))
        return AGENT_IPC_INVALID;
    *frame_length = AGENT_IPC_HEADER_SIZE + RENEW_REQUEST_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_renew_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_renew_request *request
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;
    uint8_t valid_flags = AGENT_IPC_RENEW_LATENCY |
                          AGENT_IPC_RENEW_LOAD | AGENT_IPC_RENEW_HEALTH;

    if (request == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_RENEW_REQUEST ||
        header.payload_length != RENEW_REQUEST_PAYLOAD_SIZE)
        return AGENT_IPC_WRONG_TYPE;
    memset(request, 0, sizeof(*request));
    request->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    request->lease_seconds = read_u32(payload);
    request->latency_ms = read_u32(payload + 4U);
    request->load_permille = read_u16(payload + 8U);
    request->update_flags = payload[10];
    request->healthy = payload[11] != 0U;
    if (payload[11] > 1U || request->request_id == 0U ||
        (request->update_flags & (uint8_t)~valid_flags) != 0U ||
        ((request->update_flags & AGENT_IPC_RENEW_LOAD) != 0U &&
         request->load_permille > 1000U) ||
        !read_string(request->route_id, sizeof(request->route_id),
                     payload + 12U) || request->route_id[0] == '\0')
        return AGENT_IPC_INVALID;
    if (!read_string(request->requester_agent,
                     sizeof(request->requester_agent),
                     payload + 12U + AGENT_IPC_ROUTE_ID_LEN))
        return AGENT_IPC_INVALID;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_unregister_request(
    const struct agent_ipc_unregister_request *request,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    if (request == NULL || frame == NULL || frame_length == NULL ||
        request->request_id == 0U || request->route_id[0] == '\0')
        return AGENT_IPC_INVALID;
    if (capacity < AGENT_IPC_HEADER_SIZE + UNREGISTER_REQUEST_PAYLOAD_SIZE)
        return AGENT_IPC_TOO_SMALL;
    write_header(frame, AGENT_IPC_UNREGISTER_REQUEST, request->request_id,
                 UNREGISTER_REQUEST_PAYLOAD_SIZE);
    if (!write_string(frame + AGENT_IPC_HEADER_SIZE,
                      AGENT_IPC_ROUTE_ID_LEN, request->route_id))
        return AGENT_IPC_INVALID;
    if (!write_string(frame + AGENT_IPC_HEADER_SIZE + AGENT_IPC_ROUTE_ID_LEN,
                      AGENT_IPC_AGENT_ID_LEN, request->requester_agent))
        return AGENT_IPC_INVALID;
    *frame_length = AGENT_IPC_HEADER_SIZE + UNREGISTER_REQUEST_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_unregister_request(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_unregister_request *request
)
{
    struct agent_ipc_header header;

    if (request == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_UNREGISTER_REQUEST ||
        header.payload_length != UNREGISTER_REQUEST_PAYLOAD_SIZE)
        return AGENT_IPC_WRONG_TYPE;
    memset(request, 0, sizeof(*request));
    request->request_id = header.request_id;
    if (request->request_id == 0U ||
        !read_string(request->route_id, sizeof(request->route_id),
                     frame + AGENT_IPC_HEADER_SIZE) ||
        request->route_id[0] == '\0') return AGENT_IPC_INVALID;
    if (!read_string(request->requester_agent,
                     sizeof(request->requester_agent),
                     frame + AGENT_IPC_HEADER_SIZE + AGENT_IPC_ROUTE_ID_LEN))
        return AGENT_IPC_INVALID;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_encode_lease_response(
    const struct agent_ipc_lease_response *response,
    uint8_t *frame,
    size_t capacity,
    size_t *frame_length
)
{
    uint8_t *payload;

    if (response == NULL || frame == NULL || frame_length == NULL ||
        response->request_id == 0U || response->route_id[0] == '\0')
        return AGENT_IPC_INVALID;
    if (capacity < AGENT_IPC_HEADER_SIZE + LEASE_RESPONSE_PAYLOAD_SIZE)
        return AGENT_IPC_TOO_SMALL;
    write_header(frame, AGENT_IPC_LEASE_RESPONSE, response->request_id,
                 LEASE_RESPONSE_PAYLOAD_SIZE);
    payload = frame + AGENT_IPC_HEADER_SIZE;
    write_u64(payload, response->generation);
    write_u32(payload + 8U, response->lease_seconds);
    payload[12] = response->removed ? 1U : 0U;
    if (!write_string(payload + 13U, AGENT_IPC_ROUTE_ID_LEN,
                      response->route_id)) return AGENT_IPC_INVALID;
    if (!write_string(payload + 13U + AGENT_IPC_ROUTE_ID_LEN,
                      AGENT_IPC_IPV6_LEN, response->public_ipv6))
        return AGENT_IPC_INVALID;
    *frame_length = AGENT_IPC_HEADER_SIZE + LEASE_RESPONSE_PAYLOAD_SIZE;
    return AGENT_IPC_OK;
}

enum agent_ipc_result agent_ipc_decode_lease_response(
    const uint8_t *frame,
    size_t frame_length,
    struct agent_ipc_lease_response *response
)
{
    struct agent_ipc_header header;
    const uint8_t *payload;

    if (response == NULL ||
        agent_ipc_decode_header(frame, frame_length, &header) != AGENT_IPC_OK ||
        header.type != AGENT_IPC_LEASE_RESPONSE ||
        header.payload_length != LEASE_RESPONSE_PAYLOAD_SIZE)
        return AGENT_IPC_WRONG_TYPE;
    memset(response, 0, sizeof(*response));
    response->request_id = header.request_id;
    payload = frame + AGENT_IPC_HEADER_SIZE;
    response->generation = read_u64(payload);
    response->lease_seconds = read_u32(payload + 8U);
    response->removed = payload[12] != 0U;
    if (payload[12] > 1U || response->request_id == 0U ||
        !read_string(response->route_id, sizeof(response->route_id),
                     payload + 13U) || response->route_id[0] == '\0' ||
        !read_string(response->public_ipv6, sizeof(response->public_ipv6),
                     payload + 13U + AGENT_IPC_ROUTE_ID_LEN))
        return AGENT_IPC_INVALID;
    return AGENT_IPC_OK;
}

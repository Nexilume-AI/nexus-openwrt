#include "agent_ipc_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t size, const char *value)
{
    int written = snprintf(target, size, "%s", value);
    assert(written >= 0);
    assert((size_t)written < size);
}

static void test_lookup_request_round_trip(void)
{
    struct agent_ipc_lookup_request input;
    struct agent_ipc_lookup_request output;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    memset(&input, 0, sizeof(input));
    input.request_id = 42U;
    set_text(input.intent, sizeof(input.intent),
             "chip.verilog.verify.lint");
    input.version = 1U;
    set_text(input.tenant, sizeof(input.tenant), "tenant-a");
    set_text(input.region, sizeof(input.region), "local");
    set_text(input.source_agent, sizeof(input.source_agent),
             "agent://tenant-a/client");
    set_text(input.target_agent, sizeof(input.target_agent),
             "agent://tenant-a/linter-exact");
    set_text(input.public_ipv6, sizeof(input.public_ipv6),
             "2001:db8:1234:5678::1");
    input.max_cost_microunits = 200000U;
    input.max_latency_ms = 3000U;
    input.min_trust_level = 80U;

    assert(agent_ipc_encode_lookup_request(
               &input, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_lookup_request(
               frame, frame_length, &output) == AGENT_IPC_OK);
    assert(output.request_id == input.request_id);
    assert(strcmp(output.intent, input.intent) == 0);
    assert(output.version == input.version);
    assert(strcmp(output.tenant, input.tenant) == 0);
    assert(strcmp(output.region, input.region) == 0);
    assert(strcmp(output.source_agent, input.source_agent) == 0);
    assert(strcmp(output.target_agent, input.target_agent) == 0);
    assert(strcmp(output.public_ipv6, input.public_ipv6) == 0);
    assert(output.max_cost_microunits == input.max_cost_microunits);
    assert(output.max_latency_ms == input.max_latency_ms);
    assert(output.min_trust_level == input.min_trust_level);
}

static void test_lookup_response_round_trip(void)
{
    struct agent_ipc_lookup_response input;
    struct agent_ipc_lookup_response output;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    memset(&input, 0, sizeof(input));
    input.request_id = 43U;
    input.found = true;
    input.generation = 19U;
    input.score = 123456U;
    set_text(input.route_id, sizeof(input.route_id),
             "00000000000000000000000000000001");
    set_text(input.origin, sizeof(input.origin),
             "agent://tenant-a/lint");
    set_text(input.endpoint, sizeof(input.endpoint),
             "https://127.0.0.1:9001/invoke");
    set_text(input.source, sizeof(input.source), "static");
    input.relay = true;
    set_text(input.relay_peer_id, sizeof(input.relay_peer_id), "relay-east");
    set_text(input.target_router_id, sizeof(input.target_router_id),
             "router-target");
    set_text(input.policy_id, sizeof(input.policy_id), "tenant-a-te");
    input.tenant_max_inflight = 8U;
    input.tenant_rate_per_second = 20U;
    input.tenant_rate_burst = 40U;

    assert(agent_ipc_encode_lookup_response(
               &input, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_lookup_response(
               frame, frame_length, &output) == AGENT_IPC_OK);
    assert(output.found);
    assert(output.request_id == input.request_id);
    assert(output.generation == input.generation);
    assert(output.score == input.score);
    assert(strcmp(output.route_id, input.route_id) == 0);
    assert(strcmp(output.origin, input.origin) == 0);
    assert(strcmp(output.endpoint, input.endpoint) == 0);
    assert(strcmp(output.source, input.source) == 0);
    assert(output.relay);
    assert(strcmp(output.relay_peer_id, "relay-east") == 0);
    assert(strcmp(output.target_router_id, "router-target") == 0);
    assert(strcmp(output.policy_id, "tenant-a-te") == 0);
    assert(output.tenant_max_inflight == 8U);
    assert(output.tenant_rate_per_second == 20U);
    assert(output.tenant_rate_burst == 40U);
}

static void test_not_found_response(void)
{
    struct agent_ipc_lookup_response input;
    struct agent_ipc_lookup_response output;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    memset(&input, 0, sizeof(input));
    input.request_id = 44U;
    input.generation = 20U;
    assert(agent_ipc_encode_lookup_response(
               &input, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_lookup_response(
               frame, frame_length, &output) == AGENT_IPC_OK);
    assert(!output.found);
    assert(output.generation == input.generation);
}

static void test_candidates_round_trip(void)
{
    struct agent_ipc_candidates_request request = {0};
    struct agent_ipc_candidates_request decoded_request;
    struct agent_ipc_candidates_response response = {0};
    struct agent_ipc_candidates_response decoded_response;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    request.lookup.request_id = 2745U;
    request.lookup.version = 1U;
    request.lookup.min_trust_level = 80U;
    set_text(request.lookup.intent, sizeof(request.lookup.intent),
             "chip.verilog.verify.lint");
    set_text(request.lookup.tenant, sizeof(request.lookup.tenant),
             "tenant-a");
    request.max_candidates = 3U;
    assert(agent_ipc_encode_candidates_request(
               &request, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_OK);
    assert(agent_ipc_decode_candidates_request(
               frame, frame_length, &decoded_request) == AGENT_IPC_OK);
    assert(decoded_request.max_candidates == 3U);
    assert(decoded_request.lookup.request_id == 2745U);

    response.request_id = 2745U;
    response.generation = 31U;
    response.count = 2U;
    set_text(response.policy_id, sizeof(response.policy_id), "retry-policy");
    response.tenant_max_inflight = 4U;
    response.tenant_rate_per_second = 10U;
    response.tenant_rate_burst = 12U;
    response.candidates[0].score = 100U;
    set_text(response.candidates[0].route_id,
             sizeof(response.candidates[0].route_id),
             "00000000000000000000000000000001");
    set_text(response.candidates[0].origin,
             sizeof(response.candidates[0].origin), "agent://first");
    set_text(response.candidates[0].endpoint,
             sizeof(response.candidates[0].endpoint),
             "http://127.0.0.1:19001/invoke");
    set_text(response.candidates[0].source,
             sizeof(response.candidates[0].source), "local");
    response.candidates[1].score = 200U;
    set_text(response.candidates[1].route_id,
             sizeof(response.candidates[1].route_id),
             "00000000000000000000000000000002");
    set_text(response.candidates[1].origin,
             sizeof(response.candidates[1].origin), "agent://second");
    set_text(response.candidates[1].endpoint,
             sizeof(response.candidates[1].endpoint),
             "http://127.0.0.1:19002/invoke");
    set_text(response.candidates[1].source,
             sizeof(response.candidates[1].source), "static");
    response.candidates[1].relay = true;
    set_text(response.candidates[1].relay_peer_id,
             sizeof(response.candidates[1].relay_peer_id), "relay-east");
    set_text(response.candidates[1].target_router_id,
             sizeof(response.candidates[1].target_router_id), "router-b");
    assert(agent_ipc_encode_candidates_response(
               &response, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_OK);
    assert(frame_length <= AGENT_IPC_MAX_FRAME_SIZE);
    assert(agent_ipc_decode_candidates_response(
               frame, frame_length, &decoded_response) == AGENT_IPC_OK);
    assert(decoded_response.count == 2U);
    assert(decoded_response.generation == 31U);
    assert(strcmp(decoded_response.candidates[1].route_id,
                  response.candidates[1].route_id) == 0);
    assert(strcmp(decoded_response.candidates[1].endpoint,
                  response.candidates[1].endpoint) == 0);
    assert(decoded_response.candidates[1].relay);
    assert(strcmp(decoded_response.candidates[1].relay_peer_id,
                  "relay-east") == 0);
    assert(strcmp(decoded_response.policy_id, "retry-policy") == 0);
    assert(decoded_response.tenant_max_inflight == 4U);

    request.max_candidates = 1U;
    assert(agent_ipc_encode_candidates_request(
               &request, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_INVALID);
}

static void test_invoke_round_trip(void)
{
    struct agent_ipc_invoke_request request = {0};
    struct agent_ipc_invoke_request decoded_request;
    struct agent_ipc_invoke_response response = {0};
    struct agent_ipc_invoke_response decoded_response;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    const char body[] = "{\"payload\":{\"value\":42}}";
    size_t frame_length = 0U;

    request.request_id = 9001U;
    request.timeout_ms = 3000U;
    request.hop_limit = 7U;
    request.max_cost_microunits = 200000U;
    request.max_latency_ms = 3000U;
    set_text(request.route_id, sizeof(request.route_id),
             "00000000000000000000000000000009");
    set_text(request.relay_peer_id, sizeof(request.relay_peer_id),
             "relay-east");
    set_text(request.target_router_id, sizeof(request.target_router_id),
             "router-b");
    set_text(request.intent, sizeof(request.intent),
             "chip.verilog.verify.lint.v1");
    set_text(request.task_id, sizeof(request.task_id), "task-9001");
    set_text(request.source_agent, sizeof(request.source_agent),
             "agent://tenant-a/client");
    set_text(request.target_agent, sizeof(request.target_agent),
             "agent://tenant-a/linter-exact");
    set_text(request.tenant, sizeof(request.tenant), "tenant-a");
    set_text(request.region, sizeof(request.region), "local");
    set_text(request.forwarding_assertion,
             sizeof(request.forwarding_assertion),
             "nfa1.router-a.payload.signature");
    request.body_length = sizeof(body) - 1U;
    memcpy(request.body, body, request.body_length);
    assert(agent_ipc_encode_invoke_request(
               &request, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_OK);
    assert(agent_ipc_decode_invoke_request(
               frame, frame_length, &decoded_request) == AGENT_IPC_OK);
    assert(decoded_request.request_id == 9001U);
    assert(decoded_request.hop_limit == 7U);
    assert(strcmp(decoded_request.target_router_id, "router-b") == 0);
    assert(strcmp(decoded_request.target_agent,
                  "agent://tenant-a/linter-exact") == 0);
    assert(strcmp(decoded_request.forwarding_assertion,
                  request.forwarding_assertion) == 0);
    assert(decoded_request.body_length == request.body_length);
    assert(memcmp(decoded_request.body, body, request.body_length) == 0);

    response.request_id = 9001U;
    response.status_code = 201U;
    set_text(response.content_type, sizeof(response.content_type),
             "application/json");
    response.body_length = 11U;
    memcpy(response.body, "{\"ok\":true}", response.body_length);
    assert(agent_ipc_encode_invoke_response(
               &response, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_OK);
    assert(agent_ipc_decode_invoke_response(
               frame, frame_length, &decoded_response) == AGENT_IPC_OK);
    assert(decoded_response.status_code == 201U);
    assert(strcmp(decoded_response.content_type, "application/json") == 0);
    assert(decoded_response.body_length == response.body_length);
    assert(memcmp(decoded_response.body, response.body,
                  response.body_length) == 0);

    request.body_length = AGENT_IPC_MAX_INVOKE_BODY + 1U;
    assert(agent_ipc_encode_invoke_request(
               &request, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_INVALID);
}

static void test_stream_round_trip(void)
{
    struct agent_ipc_invoke_request request = {0};
    struct agent_ipc_invoke_request decoded_request;
    struct agent_ipc_stream_start start = {0};
    struct agent_ipc_stream_start decoded_start;
    struct agent_ipc_stream_data data = {0};
    struct agent_ipc_stream_data decoded_data;
    struct agent_ipc_stream_end end = {0};
    struct agent_ipc_stream_end decoded_end;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;
    const char body[] = "{\"payload\":{\"stream\":true}}";
    const char event[] = "event: token\ndata: hello\n\n";

    request.request_id = 9100U;
    request.timeout_ms = 15000U;
    request.hop_limit = 6U;
    request.max_latency_ms = 15000U;
    set_text(request.route_id, sizeof(request.route_id),
             "00000000000000000000000000000010");
    set_text(request.relay_peer_id, sizeof(request.relay_peer_id),
             "relay-east");
    set_text(request.target_router_id, sizeof(request.target_router_id),
             "router-stream");
    set_text(request.intent, sizeof(request.intent), "chat.stream.v1");
    set_text(request.task_id, sizeof(request.task_id), "task-stream");
    set_text(request.source_agent, sizeof(request.source_agent),
             "agent://tenant-a/client");
    set_text(request.tenant, sizeof(request.tenant), "tenant-a");
    set_text(request.region, sizeof(request.region), "local");
    request.body_length = sizeof(body) - 1U;
    memcpy(request.body, body, request.body_length);
    assert(agent_ipc_encode_stream_request(
               &request, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_stream_request(
               frame, frame_length, &decoded_request) == AGENT_IPC_OK);
    assert(decoded_request.streaming);
    assert(decoded_request.body_length == request.body_length);

    start.request_id = request.request_id;
    start.status_code = 200U;
    set_text(start.content_type, sizeof(start.content_type),
             "text/event-stream");
    assert(agent_ipc_encode_stream_start(
               &start, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_stream_start(
               frame, frame_length, &decoded_start) == AGENT_IPC_OK);
    assert(decoded_start.status_code == 200U);

    data.request_id = request.request_id;
    data.data_length = sizeof(event) - 1U;
    memcpy(data.data, event, data.data_length);
    assert(agent_ipc_encode_stream_data(
               &data, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_stream_data(
               frame, frame_length, &decoded_data) == AGENT_IPC_OK);
    assert(decoded_data.data_length == data.data_length);
    assert(memcmp(decoded_data.data, event, data.data_length) == 0);

    end.request_id = request.request_id;
    end.total_bytes = data.data_length;
    assert(agent_ipc_encode_stream_end(
               &end, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_stream_end(
               frame, frame_length, &decoded_end) == AGENT_IPC_OK);
    assert(decoded_end.total_bytes == data.data_length);

    data.data_length = AGENT_IPC_MAX_STREAM_DATA + 1U;
    assert(agent_ipc_encode_stream_data(
               &data, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_INVALID);
}

static void test_error_and_malformed_frames(void)
{
    struct agent_ipc_error_response input;
    struct agent_ipc_error_response output;
    struct agent_ipc_header header;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    memset(&input, 0, sizeof(input));
    input.request_id = 45U;
    input.code = 400U;
    set_text(input.message, sizeof(input.message), "invalid lookup request");
    assert(agent_ipc_encode_error_response(
               &input, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_error_response(
               frame, frame_length, &output) == AGENT_IPC_OK);
    assert(output.code == input.code);
    assert(strcmp(output.message, input.message) == 0);

    assert(agent_ipc_decode_header(frame, 5U, &header) ==
           AGENT_IPC_TOO_SMALL);
    frame[0] = 0U;
    assert(agent_ipc_decode_header(frame, frame_length, &header) ==
           AGENT_IPC_INVALID);
}

static void test_registration_round_trip(void)
{
    struct agent_ipc_register_request registration = {0};
    struct agent_ipc_register_request decoded_registration;
    struct agent_ipc_renew_request renewal = {0};
    struct agent_ipc_renew_request decoded_renewal;
    struct agent_ipc_unregister_request removal = {0};
    struct agent_ipc_unregister_request decoded_removal;
    struct agent_ipc_lease_response lease = {0};
    struct agent_ipc_lease_response decoded_lease;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    registration.request_id = 9200U;
    registration.version = 1U;
    registration.lease_seconds = 30U;
    registration.cost_microunits = 2500U;
    registration.latency_ms = 12U;
    registration.trust_level = 75U;
    registration.load_permille = 100U;
    registration.request_public_ipv6 = true;
    set_text(registration.intent, sizeof(registration.intent),
             "chip.verilog.verify.lint.v1");
    set_text(registration.origin, sizeof(registration.origin),
             "agent://tenant-a/linter");
    set_text(registration.endpoint, sizeof(registration.endpoint),
             "https://agent-a.example.test:7443/invoke");
    set_text(registration.tenant, sizeof(registration.tenant), "tenant-a");
    set_text(registration.region, sizeof(registration.region), "lan-a");
    registration.cloud.present = true;
    registration.cloud.publish = true;
    registration.cloud.tool_present = true;
    registration.cloud.task = true;
    registration.cloud.continuable = true;
    registration.cloud.recovery_protocol = 1U;
    registration.cloud.interactive = true;
    registration.cloud.computer_present = true;
    registration.cloud.computer_requirement = AGENT_IPC_COMPUTER_REQUIRED;
    registration.cloud.workspace_capabilities =
        AGENT_IPC_WORKSPACE_FILES_LIST |
        AGENT_IPC_WORKSPACE_FILES_READ |
        AGENT_IPC_WORKSPACE_FILES_WRITE |
        AGENT_IPC_WORKSPACE_COMMAND_EXECUTE |
        AGENT_IPC_WORKSPACE_BROWSER_CONTROL;
    registration.cloud.mobile_present = true;
    registration.cloud.mobile_requirement = AGENT_IPC_MOBILE_REQUIRED;
    registration.cloud.mobile_capabilities =
        AGENT_IPC_MOBILE_OBSERVE |
        AGENT_IPC_MOBILE_SCREEN_CAPTURE |
        AGENT_IPC_MOBILE_TAP |
        AGENT_IPC_MOBILE_TYPE_TEXT;
    registration.cloud.mobile_scopes =
        AGENT_IPC_MOBILE_OBSERVE |
        AGENT_IPC_MOBILE_TYPE_TEXT;
    set_text(registration.cloud.agent_name,
             sizeof(registration.cloud.agent_name), "LAN linter");
    set_text(registration.cloud.manifest_digest,
             sizeof(registration.cloud.manifest_digest),
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    set_text(registration.cloud.tool_name,
             sizeof(registration.cloud.tool_name), "lint_verilog");
    set_text(registration.cloud.tool_title,
             sizeof(registration.cloud.tool_title), "Lint Verilog");
    set_text(registration.cloud.tool_description,
             sizeof(registration.cloud.tool_description),
             "Validate one Verilog source file.");
    set_text(registration.cloud.tool_input_schema,
             sizeof(registration.cloud.tool_input_schema),
             "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"}}}");
    assert(agent_ipc_encode_register_request(
               &registration, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_OK);
    assert(agent_ipc_decode_register_request(
               frame, frame_length, &decoded_registration) == AGENT_IPC_OK);
    assert(decoded_registration.request_id == 9200U);
    assert(decoded_registration.lease_seconds == 30U);
    assert(strcmp(decoded_registration.intent, registration.intent) == 0);
    assert(strcmp(decoded_registration.endpoint, registration.endpoint) == 0);
    assert(decoded_registration.request_public_ipv6);
    assert(decoded_registration.cloud.present);
    assert(decoded_registration.cloud.publish);
    assert(decoded_registration.cloud.tool_present);
    assert(decoded_registration.cloud.task);
    assert(decoded_registration.cloud.continuable);
    assert(decoded_registration.cloud.recovery_protocol == 1U);
    assert(decoded_registration.cloud.interactive);
    assert(decoded_registration.cloud.computer_present);
    assert(decoded_registration.cloud.computer_requirement ==
           AGENT_IPC_COMPUTER_REQUIRED);
    assert(decoded_registration.cloud.workspace_capabilities ==
           registration.cloud.workspace_capabilities);
    assert(decoded_registration.cloud.mobile_present);
    assert(decoded_registration.cloud.mobile_requirement ==
           AGENT_IPC_MOBILE_REQUIRED);
    assert(decoded_registration.cloud.mobile_capabilities ==
           registration.cloud.mobile_capabilities);
    assert(decoded_registration.cloud.mobile_scopes ==
           registration.cloud.mobile_scopes);
    assert(strcmp(decoded_registration.cloud.agent_name, "LAN linter") == 0);
    assert(strcmp(decoded_registration.cloud.tool_name, "lint_verilog") == 0);
    assert(strcmp(decoded_registration.cloud.tool_input_schema,
                  registration.cloud.tool_input_schema) == 0);

    {
        size_t v12_frame_length = 0U;
        int encode_status;

        registration.cloud.workspace_capabilities &=
            (uint16_t)~AGENT_IPC_WORKSPACE_BROWSER_CONTROL;
        encode_status = agent_ipc_encode_register_request(
            &registration, frame, sizeof(frame), &v12_frame_length);
        assert(encode_status == AGENT_IPC_OK);
        if (encode_status != AGENT_IPC_OK) return;
        v12_frame_length -= 2U;
        frame[4] = 0U;
        frame[5] = 12U;
        frame[15] = (uint8_t)(frame[15] - 2U);
        assert(agent_ipc_decode_register_request(
                   frame, v12_frame_length, &decoded_registration) ==
               AGENT_IPC_OK);
        assert(decoded_registration.cloud.continuable ==
               decoded_registration.cloud.resumable);
        assert(decoded_registration.cloud.recovery_protocol == 0U);

        frame[4] = 0U;
        frame[5] = 11U;
        assert(agent_ipc_decode_register_request(
                   frame, v12_frame_length, &decoded_registration) ==
               AGENT_IPC_OK);
        assert((decoded_registration.cloud.workspace_capabilities &
                AGENT_IPC_WORKSPACE_BROWSER_CONTROL) == 0U);
        frame_length = v12_frame_length;
    }

    {
        size_t v10_payload_length = frame_length - AGENT_IPC_HEADER_SIZE - 6U;
        size_t v10_frame_length = AGENT_IPC_HEADER_SIZE + v10_payload_length;

        frame[4] = 0U;
        frame[5] = 10U;
        frame[12] = (uint8_t)(v10_payload_length >> 24U);
        frame[13] = (uint8_t)(v10_payload_length >> 16U);
        frame[14] = (uint8_t)(v10_payload_length >> 8U);
        frame[15] = (uint8_t)v10_payload_length;
        assert(agent_ipc_decode_register_request(
                   frame, v10_frame_length, &decoded_registration) ==
               AGENT_IPC_OK);
        assert(decoded_registration.cloud.computer_present);
        assert(!decoded_registration.cloud.mobile_present);
        assert(decoded_registration.cloud.mobile_capabilities == 0U);

        size_t v9_payload_length = v10_payload_length - 4U;
        size_t v9_frame_length = AGENT_IPC_HEADER_SIZE + v9_payload_length;

        frame[4] = 0U;
        frame[5] = 9U;
        frame[12] = (uint8_t)(v9_payload_length >> 24U);
        frame[13] = (uint8_t)(v9_payload_length >> 16U);
        frame[14] = (uint8_t)(v9_payload_length >> 8U);
        frame[15] = (uint8_t)v9_payload_length;
        assert(agent_ipc_decode_register_request(
                   frame, v9_frame_length, &decoded_registration) ==
               AGENT_IPC_OK);
        assert(decoded_registration.cloud.present);
        assert(!decoded_registration.cloud.computer_present);
        assert(decoded_registration.cloud.workspace_capabilities == 0U);
    }

    {
        size_t v8_payload_length = 25U + AGENT_IPC_ROUTE_ID_LEN +
            AGENT_IPC_INTENT_LEN + (2U * AGENT_IPC_URI_LEN) +
            AGENT_IPC_TENANT_LEN + AGENT_IPC_REGION_LEN;
        size_t v8_frame_length = AGENT_IPC_HEADER_SIZE + v8_payload_length;

        frame[4] = 0U;
        frame[5] = 8U;
        frame[12] = (uint8_t)(v8_payload_length >> 24U);
        frame[13] = (uint8_t)(v8_payload_length >> 16U);
        frame[14] = (uint8_t)(v8_payload_length >> 8U);
        frame[15] = (uint8_t)v8_payload_length;
        assert(agent_ipc_decode_register_request(
                   frame, v8_frame_length, &decoded_registration) ==
               AGENT_IPC_OK);
        assert(!decoded_registration.cloud.present);
        assert(!decoded_registration.cloud.computer_present);
        assert(strcmp(decoded_registration.intent, registration.intent) == 0);
    }

    registration.cloud.computer_requirement = AGENT_IPC_COMPUTER_DISABLED;
    assert(agent_ipc_encode_register_request(
               &registration, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_INVALID);
    registration.cloud.computer_requirement = AGENT_IPC_COMPUTER_REQUIRED;
    registration.cloud.mobile_scopes = AGENT_IPC_MOBILE_OPEN_APP;
    assert(agent_ipc_encode_register_request(
               &registration, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_INVALID);
    registration.cloud.mobile_scopes =
        AGENT_IPC_MOBILE_OBSERVE | AGENT_IPC_MOBILE_TYPE_TEXT;
    registration.cloud.recovery_protocol = 2U;
    assert(agent_ipc_encode_register_request(
               &registration, frame, sizeof(frame), &frame_length) ==
           AGENT_IPC_INVALID);
    registration.cloud.recovery_protocol = 1U;

    renewal.request_id = 9201U;
    renewal.lease_seconds = 45U;
    renewal.latency_ms = 18U;
    renewal.load_permille = 300U;
    renewal.healthy = true;
    renewal.update_flags = AGENT_IPC_RENEW_LATENCY |
                           AGENT_IPC_RENEW_LOAD |
                           AGENT_IPC_RENEW_HEALTH;
    set_text(renewal.route_id, sizeof(renewal.route_id),
             "00000000000000000000000000000020");
    set_text(renewal.requester_agent, sizeof(renewal.requester_agent),
             "agent://tenant-a/linter");
    assert(agent_ipc_encode_renew_request(
               &renewal, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_renew_request(
               frame, frame_length, &decoded_renewal) == AGENT_IPC_OK);
    assert(decoded_renewal.update_flags == renewal.update_flags);
    assert(decoded_renewal.load_permille == 300U);
    assert(strcmp(decoded_renewal.requester_agent,
                  renewal.requester_agent) == 0);

    removal.request_id = 9202U;
    set_text(removal.route_id, sizeof(removal.route_id), renewal.route_id);
    set_text(removal.requester_agent, sizeof(removal.requester_agent),
             renewal.requester_agent);
    assert(agent_ipc_encode_unregister_request(
               &removal, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_unregister_request(
               frame, frame_length, &decoded_removal) == AGENT_IPC_OK);
    assert(strcmp(decoded_removal.route_id, removal.route_id) == 0);
    assert(strcmp(decoded_removal.requester_agent,
                  removal.requester_agent) == 0);

    lease.request_id = 9202U;
    lease.generation = 77U;
    lease.removed = true;
    set_text(lease.route_id, sizeof(lease.route_id), removal.route_id);
    set_text(lease.public_ipv6, sizeof(lease.public_ipv6),
             "2001:db8:1234:5678::20");
    assert(agent_ipc_encode_lease_response(
               &lease, frame, sizeof(frame), &frame_length) == AGENT_IPC_OK);
    assert(agent_ipc_decode_lease_response(
               frame, frame_length, &decoded_lease) == AGENT_IPC_OK);
    assert(decoded_lease.removed);
    assert(decoded_lease.generation == 77U);
    assert(strcmp(decoded_lease.public_ipv6, lease.public_ipv6) == 0);
}

int main(void)
{
    test_lookup_request_round_trip();
    test_lookup_response_round_trip();
    test_not_found_response();
    test_candidates_round_trip();
    test_invoke_round_trip();
    test_stream_round_trip();
    test_registration_round_trip();
    test_error_and_malformed_frames();
    puts("agent IPC protocol tests passed");
    return 0;
}

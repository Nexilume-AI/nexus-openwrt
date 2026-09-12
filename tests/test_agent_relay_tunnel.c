#include "agent_relay_tunnel.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct agent_relay_tunnel_message open_message(void)
{
    struct agent_relay_tunnel_message message;

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_OPEN;
    message.stream_id = 1U;
    message.sequence = 1U;
    message.streaming = true;
    message.hop_limit = 8U;
    message.max_cost_microunits = 200000U;
    message.max_latency_ms = 3000U;
    snprintf(message.target_router_id, sizeof(message.target_router_id),
             "%s", "router-b");
    snprintf(message.intent_class, sizeof(message.intent_class), "%s",
             "chip.verilog.verify.lint.v1");
    snprintf(message.task_id, sizeof(message.task_id), "%s", "task-001");
    snprintf(message.source_agent, sizeof(message.source_agent), "%s",
             "agent://tenant-a/caller");
    snprintf(message.target_agent, sizeof(message.target_agent), "%s",
             "agent://tenant-a/linter-exact");
    snprintf(message.tenant, sizeof(message.tenant), "%s", "tenant-a");
    snprintf(message.region, sizeof(message.region), "%s", "local");
    snprintf(message.forwarding_assertion,
             sizeof(message.forwarding_assertion), "%s",
             "nfa1.router-a.payload.signature");
    return message;
}

static void test_open_round_trip(void)
{
    struct agent_relay_tunnel_message input = open_message();
    struct agent_relay_tunnel_message output;
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    assert(agent_relay_tunnel_frame_encode(
               &input, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(written > AGENT_RELAY_TUNNEL_HEADER_SIZE);
    assert(agent_relay_tunnel_frame_decode(frame, written, &output) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(output.type == AGENT_RELAY_TUNNEL_OPEN);
    assert(output.stream_id == 1U);
    assert(output.sequence == 1U);
    assert(output.streaming);
    assert(output.hop_limit == 8U);
    assert(output.max_cost_microunits == 200000U);
    assert(output.max_latency_ms == 3000U);
    assert(strcmp(output.target_router_id, "router-b") == 0);
    assert(strcmp(output.intent_class,
                  "chip.verilog.verify.lint.v1") == 0);
    assert(strcmp(output.task_id, "task-001") == 0);
    assert(strcmp(output.source_agent, "agent://tenant-a/caller") == 0);
    assert(strcmp(output.target_agent,
                  "agent://tenant-a/linter-exact") == 0);
    assert(strcmp(output.forwarding_assertion,
                  "nfa1.router-a.payload.signature") == 0);
}

static void test_legacy_open_without_assertion(void)
{
    struct agent_relay_tunnel_message input = open_message();
    struct agent_relay_tunnel_message output;
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    input.forwarding_assertion[0] = '\0';
    input.target_agent[0] = '\0';
    assert(agent_relay_tunnel_frame_encode(
               &input, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_tunnel_frame_decode(frame, written, &output) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(output.forwarding_assertion[0] == '\0');
    assert(output.target_agent[0] == '\0');
}

static void test_region_wildcard_round_trip(void)
{
    struct agent_relay_tunnel_message input = open_message();
    struct agent_relay_tunnel_message output;
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    snprintf(input.region, sizeof(input.region), "%s", "*");
    assert(agent_relay_tunnel_frame_encode(
               &input, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_tunnel_frame_decode(frame, written, &output) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(strcmp(output.region, "*") == 0);
}
static void test_data_and_control_round_trip(void)
{
    struct agent_relay_tunnel_message input;
    struct agent_relay_tunnel_message output;
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    memset(&input, 0, sizeof(input));
    input.type = AGENT_RELAY_TUNNEL_DATA;
    input.stream_id = 9U;
    input.sequence = 3U;
    memcpy(input.data, "prompt-body", 11U);
    input.data_length = 11U;
    assert(agent_relay_tunnel_frame_encode(
               &input, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_tunnel_frame_decode(frame, written, &output) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(output.data_length == 11U);
    assert(memcmp(output.data, "prompt-body", 11U) == 0);

    memset(&input, 0, sizeof(input));
    input.type = AGENT_RELAY_TUNNEL_WINDOW_UPDATE;
    input.stream_id = 9U;
    input.sequence = 4U;
    input.credit_bytes = 4096U;
    assert(agent_relay_tunnel_frame_encode(
               &input, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_tunnel_frame_decode(frame, written, &output) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(output.credit_bytes == 4096U);

    memset(&input, 0, sizeof(input));
    input.type = AGENT_RELAY_TUNNEL_RESPONSE_START;
    input.stream_id = 9U;
    input.sequence = 5U;
    input.status_code = 200U;
    assert(agent_relay_tunnel_frame_encode(
               &input, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_tunnel_frame_decode(frame, written, &output) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(output.type == AGENT_RELAY_TUNNEL_RESPONSE_START);
    assert(output.status_code == 200U);
}

struct capture {
    size_t frames;
    enum agent_relay_tunnel_type type[2];
};

static bool capture_frame(
    const uint8_t *frame,
    size_t frame_size,
    void *context
)
{
    struct capture *capture = context;
    struct agent_relay_tunnel_message message;

    if (capture->frames >= 2U ||
        agent_relay_tunnel_frame_decode(frame, frame_size, &message) !=
            AGENT_RELAY_TUNNEL_OK) {
        return false;
    }
    capture->type[capture->frames++] = message.type;
    return true;
}

static void test_fragmented_ingress(void)
{
    struct agent_relay_tunnel_message first = open_message();
    struct agent_relay_tunnel_message second;
    struct agent_relay_tunnel_ingress ingress;
    struct capture capture;
    uint8_t bytes[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE * 2U];
    size_t first_size = 0U;
    size_t second_size = 0U;
    size_t emitted = 0U;

    memset(&second, 0, sizeof(second));
    second.type = AGENT_RELAY_TUNNEL_PING;
    second.sequence = 77U;
    assert(agent_relay_tunnel_frame_encode(
               &first, bytes, sizeof(bytes), &first_size) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_tunnel_frame_encode(
               &second, bytes + first_size, sizeof(bytes) - first_size,
               &second_size) == AGENT_RELAY_TUNNEL_OK);
    memset(&capture, 0, sizeof(capture));
    agent_relay_tunnel_ingress_init(&ingress);
    assert(agent_relay_tunnel_ingress_feed(
               &ingress, bytes, 7U, capture_frame, &capture, &emitted) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(emitted == 0U);
    assert(agent_relay_tunnel_ingress_feed(
               &ingress, bytes + 7U, first_size + second_size - 7U,
               capture_frame, &capture, &emitted) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(emitted == 2U);
    assert(capture.frames == 2U);
    assert(capture.type[0] == AGENT_RELAY_TUNNEL_OPEN);
    assert(capture.type[1] == AGENT_RELAY_TUNNEL_PING);
}

static void test_mux_lifecycle_and_flow_control(void)
{
    struct agent_relay_mux node;
    struct agent_relay_mux relay;
    struct agent_relay_tunnel_message message = open_message();
    uint32_t stream_id;

    assert(agent_relay_mux_init(&node, true, 4U, 4096U));
    assert(agent_relay_mux_init(&relay, false, 4U, 4096U));
    message.stream_id = 0U;
    message.sequence = 0U;
    assert(agent_relay_mux_open(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert((message.stream_id & 1U) == 1U);
    stream_id = message.stream_id;
    assert(agent_relay_mux_on_receive(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_ACCEPT;
    message.stream_id = stream_id;
    message.sequence = 1U;
    message.status_code = 200U;
    message.credit_bytes = 4096U;
    assert(agent_relay_mux_on_send(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_DATA;
    message.stream_id = stream_id;
    message.sequence = 2U;
    memset(message.data, 'x', sizeof(message.data));
    message.data_length = sizeof(message.data);
    assert(agent_relay_mux_on_send(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    message.sequence = 3U;
    message.data_length = 1U;
    assert(agent_relay_mux_on_send(&node, &message) ==
           AGENT_RELAY_TUNNEL_FLOW_CONTROL);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_WINDOW_UPDATE;
    message.stream_id = stream_id;
    message.sequence = 2U;
    message.credit_bytes = 4096U;
    assert(agent_relay_mux_on_send(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_END;
    message.stream_id = stream_id;
    message.sequence = 3U;
    assert(agent_relay_mux_on_send(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    message.sequence = 3U;
    assert(agent_relay_mux_on_send(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(node.active_streams == 0U);
    assert(relay.active_streams == 0U);

    /* A credit frame sent before the opposite END can arrive after both ENDs
     * have crossed and the receiver has released the stream. It is harmless
     * and must not terminate the multiplexed Relay session. */
    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_WINDOW_UPDATE;
    message.stream_id = stream_id;
    message.sequence = 4U;
    message.credit_bytes = 64U;
    assert(agent_relay_mux_on_receive(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(relay.active_streams == 0U);

    /* Cancellation can also cross the final END and arrive after release. */
    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_RESET;
    message.stream_id = stream_id;
    message.sequence = 5U;
    message.reset_code = 1U;
    assert(agent_relay_mux_on_receive(&relay, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(relay.active_streams == 0U);

    message.type = AGENT_RELAY_TUNNEL_WINDOW_UPDATE;
    message.stream_id = stream_id + 2U;
    message.credit_bytes = 64U;
    assert(agent_relay_mux_on_receive(&relay, &message) ==
           AGENT_RELAY_TUNNEL_INVALID_STATE);
}

static void test_mux_parity_replay_and_limit(void)
{
    struct agent_relay_mux node;
    struct agent_relay_tunnel_message message = open_message();

    assert(agent_relay_mux_init(&node, true, 1U, 4096U));
    message.stream_id = 2U;
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    message.stream_id = 4U;
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_STREAM_LIMIT);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_RESET;
    message.stream_id = 2U;
    message.sequence = 1U;
    message.reset_code = 1U;
    assert(agent_relay_mux_on_send(&node, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    message = open_message();
    message.stream_id = 2U;
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_INVALID_STATE);
    message.stream_id = 3U;
    assert(agent_relay_mux_on_receive(&node, &message) ==
           AGENT_RELAY_TUNNEL_INVALID_STATE);
}

static void test_direct_peer_reverse_direction(void)
{
    struct agent_relay_mux dialer;
    struct agent_relay_mux acceptor;
    struct agent_relay_tunnel_message message = open_message();
    uint32_t stream_id;

    assert(agent_relay_mux_init(&dialer, true, 4U, 4096U));
    assert(agent_relay_mux_init(&acceptor, false, 4U, 4096U));
    snprintf(message.target_router_id, sizeof(message.target_router_id),
             "%s", "router-a");
    assert(agent_relay_mux_open(&acceptor, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert((message.stream_id & 1U) == 0U);
    stream_id = message.stream_id;
    assert(agent_relay_mux_on_receive(&dialer, &message) ==
           AGENT_RELAY_TUNNEL_OK);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_ACCEPT;
    message.stream_id = stream_id;
    message.sequence = 1U;
    message.status_code = 200U;
    message.credit_bytes = 65536U;
    assert(agent_relay_mux_on_send(&dialer, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&acceptor, &message) ==
           AGENT_RELAY_TUNNEL_OK);

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_END;
    message.stream_id = stream_id;
    message.sequence = 2U;
    assert(agent_relay_mux_on_send(&acceptor, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&dialer, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    message.sequence = 2U;
    assert(agent_relay_mux_on_send(&dialer, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(agent_relay_mux_on_receive(&acceptor, &message) ==
           AGENT_RELAY_TUNNEL_OK);
    assert(dialer.active_streams == 0U);
    assert(acceptor.active_streams == 0U);
}

static void test_malformed_frame_rejected(void)
{
    struct agent_relay_tunnel_message message = open_message();
    struct agent_relay_tunnel_message decoded;
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    assert(agent_relay_tunnel_frame_encode(
               &message, frame, sizeof(frame), &written) ==
           AGENT_RELAY_TUNNEL_OK);
    frame[0] = 0U;
    assert(agent_relay_tunnel_frame_decode(frame, written, &decoded) ==
           AGENT_RELAY_TUNNEL_INVALID_FRAME);
}

int main(void)
{
    test_open_round_trip();
    test_legacy_open_without_assertion();
    test_region_wildcard_round_trip();
    test_data_and_control_round_trip();
    test_fragmented_ingress();
    test_mux_lifecycle_and_flow_control();
    test_mux_parity_replay_and_limit();
    test_direct_peer_reverse_direction();
    test_malformed_frame_rejected();
    puts("agent relay tunnel tests passed");
    return 0;
}

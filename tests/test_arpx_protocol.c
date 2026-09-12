#include "agent_arpx_protocol.h"
#include "agent_peer_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *value)
{
    int written = snprintf(target, capacity, "%s", value);

    assert(written >= 0);
    assert((size_t)written < capacity);
}

static struct agent_arpx_message make_open(void)
{
    struct agent_arpx_message message;

    memset(&message, 0, sizeof(message));
    message.version = AGENT_ARPX_VERSION;
    message.type = AGENT_ARPX_OPEN;
    set_text(message.router_id, sizeof(message.router_id), "router-a");
    set_text(message.domain_id, sizeof(message.domain_id), "eda.example");
    message.boot_epoch = 42U;
    message.sequence = 1U;
    message.heartbeat_ms = 2000U;
    return message;
}

static struct agent_arpx_message make_heartbeat(uint64_t sequence)
{
    struct agent_arpx_message message = make_open();

    message.type = AGENT_ARPX_HEARTBEAT;
    message.sequence = sequence;
    message.heartbeat_ms = 0U;
    return message;
}

static struct agent_arpx_message make_update(uint64_t sequence)
{
    struct agent_arpx_message message = make_heartbeat(sequence);

    message.type = AGENT_ARPX_CAPABILITY_UPDATE;
    set_text(message.route_id, sizeof(message.route_id),
             "0123456789abcdef0123456789abcdef");
    set_text(message.intent, sizeof(message.intent),
             "chip.verilog.verify.lint.v1");
    message.capability_version = 1U;
    set_text(message.origin, sizeof(message.origin), "agent://lint-a");
    set_text(message.endpoint, sizeof(message.endpoint),
             "http://192.0.2.1:8080/invoke");
    set_text(message.tenant, sizeof(message.tenant), "eda");
    set_text(message.region, sizeof(message.region), "local");
    message.cost_microunits = 200000U;
    message.latency_ms = 30U;
    message.trust_level = 80U;
    message.load_permille = 250U;
    message.remaining_lease_ms = 30000U;
    message.path_length = 1U;
    set_text(message.path[0], sizeof(message.path[0]), "router-a");
    return message;
}

static struct agent_arpx_message make_snapshot(
    enum agent_arpx_message_type type,
    uint64_t sequence,
    uint64_t snapshot_id
)
{
    struct agent_arpx_message message = make_heartbeat(sequence);

    message.type = type;
    message.snapshot_id = snapshot_id;
    return message;
}

static struct agent_peer_session_config make_session_config(void)
{
    struct agent_peer_session_config config;

    memset(&config, 0, sizeof(config));
    set_text(config.expected_router_id, sizeof(config.expected_router_id),
             "router-a");
    set_text(config.expected_domain_id, sizeof(config.expected_domain_id),
             "eda.example");
    config.open_timeout_ms = 5000U;
    config.heartbeat_miss_limit = 3U;
    config.initial_backoff_ms = 250U;
    config.max_backoff_ms = 30000U;
    return config;
}

static void test_open_round_trip(void)
{
    struct agent_arpx_message input = make_open();
    struct agent_arpx_message output;
    uint8_t frame[AGENT_ARPX_MAX_FRAME_SIZE];
    size_t written = 0U;

    assert(agent_arpx_frame_encode(&input, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    assert(written > AGENT_ARPX_FRAME_HEADER_SIZE);
    assert(written <= AGENT_ARPX_MAX_FRAME_SIZE);
    assert(agent_arpx_frame_decode(frame, written, &output) == AGENT_ARPX_OK);
    assert(output.version == AGENT_ARPX_VERSION);
    assert(output.type == AGENT_ARPX_OPEN);
    assert(strcmp(output.router_id, "router-a") == 0);
    assert(strcmp(output.domain_id, "eda.example") == 0);
    assert(output.boot_epoch == 42U);
    assert(output.sequence == 1U);
    assert(output.heartbeat_ms == 2000U);
}

static void test_node_fixture_open_compatibility(void)
{
    static const uint8_t frame[] = {
        0x00, 0x00, 0x00, 0x39, 0xa7, 0x00, 0x01, 0x01,
        0x01, 0x02, 0x72, 0x72, 0x6f, 0x75, 0x74, 0x65,
        0x72, 0x2d, 0x72, 0x65, 0x6c, 0x61, 0x79, 0x2d,
        0x70, 0x33, 0x37, 0x2d, 0x31, 0x03, 0x6e, 0x72,
        0x65, 0x6c, 0x61, 0x79, 0x2e, 0x70, 0x33, 0x37,
        0x2e, 0x74, 0x65, 0x73, 0x74, 0x04, 0x1b, 0x00,
        0x06, 0x58, 0x57, 0xcd, 0xe5, 0x00, 0x00, 0x05,
        0x01, 0x06, 0x19, 0x03, 0xe8
    };
    struct agent_arpx_message output;

    assert(agent_arpx_frame_decode(frame, sizeof(frame), &output) ==
           AGENT_ARPX_OK);
    assert(output.type == AGENT_ARPX_OPEN);
    assert(strcmp(output.router_id, "router-relay-p37-1") == 0);
    assert(strcmp(output.domain_id, "relay.p37.test") == 0);
    assert(output.sequence == 1U);
    assert(output.heartbeat_ms == 1000U);
}

static void test_heartbeat_round_trip(void)
{
    struct agent_arpx_message input = make_heartbeat(9U);
    struct agent_arpx_message output;
    uint8_t frame[512];
    size_t written = 0U;

    assert(agent_arpx_frame_encode(&input, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    assert(agent_arpx_frame_decode(frame, written, &output) == AGENT_ARPX_OK);
    assert(output.type == AGENT_ARPX_HEARTBEAT);
    assert(output.sequence == 9U);
    assert(output.heartbeat_ms == 0U);
}

static void test_update_and_withdraw_round_trip(void)
{
    struct agent_arpx_message input = make_update(2U);
    struct agent_arpx_message output;
    uint8_t frame[AGENT_ARPX_MAX_FRAME_SIZE];
    size_t written = 0U;

    assert(agent_arpx_frame_encode(&input, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    assert(agent_arpx_frame_decode(frame, written, &output) == AGENT_ARPX_OK);
    assert(output.type == AGENT_ARPX_CAPABILITY_UPDATE);
    assert(strcmp(output.route_id, input.route_id) == 0);
    assert(strcmp(output.intent, input.intent) == 0);
    assert(output.capability_version == 1U);
    assert(output.cost_microunits == 200000U);
    assert(output.remaining_lease_ms == 30000U);
    assert(output.path_length == 1U);
    assert(strcmp(output.path[0], "router-a") == 0);

    memset(&input, 0, sizeof(input));
    input = make_heartbeat(3U);
    input.type = AGENT_ARPX_CAPABILITY_WITHDRAW;
    set_text(input.route_id, sizeof(input.route_id),
             "0123456789abcdef0123456789abcdef");
    assert(agent_arpx_frame_encode(&input, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    assert(agent_arpx_frame_decode(frame, written, &output) == AGENT_ARPX_OK);
    assert(output.type == AGENT_ARPX_CAPABILITY_WITHDRAW);
    assert(strcmp(output.route_id, input.route_id) == 0);
}

static void test_snapshot_round_trip(void)
{
    struct agent_arpx_message input = make_snapshot(
        AGENT_ARPX_SNAPSHOT_REQUEST, 4U, 9001U);
    struct agent_arpx_message output;
    uint8_t frame[512];
    size_t written = 0U;

    assert(agent_arpx_frame_encode(&input, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    assert(agent_arpx_frame_decode(frame, written, &output) == AGENT_ARPX_OK);
    assert(output.type == AGENT_ARPX_SNAPSHOT_REQUEST);
    assert(output.snapshot_id == 9001U);

    input.type = AGENT_ARPX_SNAPSHOT_END;
    input.sequence = 5U;
    assert(agent_arpx_frame_encode(&input, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    assert(agent_arpx_frame_decode(frame, written, &output) == AGENT_ARPX_OK);
    assert(output.type == AGENT_ARPX_SNAPSHOT_END);
    assert(output.snapshot_id == 9001U);
}

static void test_message_validation(void)
{
    struct agent_arpx_message message = make_open();

    message.version = 2U;
    assert(agent_arpx_message_validate(&message) ==
           AGENT_ARPX_INVALID_MESSAGE);
    message = make_open();
    message.sequence = 2U;
    assert(agent_arpx_message_validate(&message) ==
           AGENT_ARPX_INVALID_MESSAGE);
    message = make_snapshot(AGENT_ARPX_SNAPSHOT_REQUEST, 2U, 0U);
    assert(agent_arpx_message_validate(&message) ==
           AGENT_ARPX_INVALID_MESSAGE);
    message = make_open();
    message.heartbeat_ms = 999U;
    assert(agent_arpx_message_validate(&message) ==
           AGENT_ARPX_INVALID_MESSAGE);
    message = make_update(2U);
    message.path_length = 2U;
    set_text(message.path[1], sizeof(message.path[1]), "router-a");
    assert(agent_arpx_message_validate(&message) ==
           AGENT_ARPX_INVALID_MESSAGE);
    message = make_open();
    set_text(message.router_id, sizeof(message.router_id), "Router-A");
    assert(agent_arpx_message_validate(&message) ==
           AGENT_ARPX_INVALID_MESSAGE);
}

static void test_strict_framing(void)
{
    struct agent_arpx_message message = make_open();
    struct agent_arpx_message output;
    uint8_t frame[512];
    size_t written = 0U;

    assert(agent_arpx_frame_encode(&message, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    frame[3]++;
    assert(agent_arpx_frame_decode(frame, written, &output) ==
           AGENT_ARPX_INVALID_FRAME);

    assert(agent_arpx_frame_encode(&message, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    frame[5] = 1U;
    assert(agent_arpx_frame_decode(frame, written, &output) ==
           AGENT_ARPX_INVALID_FRAME);

    assert(agent_arpx_frame_encode(&message, frame, sizeof(frame), &written) ==
           AGENT_ARPX_OK);
    frame[written] = 0U;
    written++;
    frame[0] = (uint8_t)((written - AGENT_ARPX_FRAME_HEADER_SIZE) >> 24U);
    frame[1] = (uint8_t)((written - AGENT_ARPX_FRAME_HEADER_SIZE) >> 16U);
    frame[2] = (uint8_t)((written - AGENT_ARPX_FRAME_HEADER_SIZE) >> 8U);
    frame[3] = (uint8_t)(written - AGENT_ARPX_FRAME_HEADER_SIZE);
    assert(agent_arpx_frame_decode(frame, written, &output) ==
           AGENT_ARPX_INVALID_FRAME);
}

static void test_session_open_and_heartbeat(void)
{
    struct agent_peer_session_config config = make_session_config();
    struct agent_peer_session session;
    struct agent_arpx_message open = make_open();
    struct agent_arpx_message heartbeat = make_heartbeat(2U);

    assert(agent_peer_session_init(&session, &config));
    assert(session.state == AGENT_PEER_SESSION_CONFIGURED);
    assert(agent_peer_session_begin(&session, 1000U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.state == AGENT_PEER_SESSION_CONNECTING);
    assert(agent_peer_session_transport_ready(&session, 1100U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.state == AGENT_PEER_SESSION_WAIT_OPEN);
    assert(agent_peer_session_on_message(&session, &open, 1200U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.state == AGENT_PEER_SESSION_ESTABLISHED);
    assert(session.deadline_ms == 7200U);
    assert(session.opens_accepted == 1U);
    assert(agent_peer_session_on_message(&session, &heartbeat, 3000U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.last_sequence == 2U);
    assert(session.deadline_ms == 9000U);
    assert(session.heartbeats_accepted == 1U);
    assert(!agent_peer_session_tick(&session, 8999U));

    heartbeat = make_update(3U);
    assert(agent_peer_session_on_message(&session, &heartbeat, 4000U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.updates_accepted == 1U);
    assert(session.last_sequence == 3U);
    heartbeat = make_snapshot(AGENT_ARPX_SNAPSHOT_REQUEST, 4U, 77U);
    assert(agent_peer_session_on_message(&session, &heartbeat, 4100U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.snapshot_requests_accepted == 1U);
    heartbeat = make_snapshot(AGENT_ARPX_SNAPSHOT_END, 5U, 77U);
    assert(agent_peer_session_on_message(&session, &heartbeat, 4200U) ==
           AGENT_PEER_SESSION_OK);
    assert(session.snapshot_ends_accepted == 1U);
}

static void test_session_identity_and_sequence_fail_closed(void)
{
    struct agent_peer_session_config config = make_session_config();
    struct agent_peer_session session;
    struct agent_arpx_message open = make_open();
    struct agent_arpx_message heartbeat;

    assert(agent_peer_session_init(&session, &config));
    assert(agent_peer_session_begin(&session, 0U) == AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_transport_ready(&session, 1U) ==
           AGENT_PEER_SESSION_OK);
    set_text(open.router_id, sizeof(open.router_id), "router-b");
    assert(agent_peer_session_on_message(&session, &open, 2U) ==
           AGENT_PEER_SESSION_IDENTITY_MISMATCH);
    assert(session.state == AGENT_PEER_SESSION_BACKOFF);
    assert(session.protocol_errors == 1U);

    assert(agent_peer_session_init(&session, &config));
    open = make_open();
    assert(agent_peer_session_begin(&session, 0U) == AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_transport_ready(&session, 1U) ==
           AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_on_message(&session, &open, 2U) ==
           AGENT_PEER_SESSION_OK);
    heartbeat = make_heartbeat(3U);
    assert(agent_peer_session_on_message(&session, &heartbeat, 3U) ==
           AGENT_PEER_SESSION_SEQUENCE_ERROR);
    assert(session.state == AGENT_PEER_SESSION_BACKOFF);

    assert(agent_peer_session_begin(&session, 4U) ==
           AGENT_PEER_SESSION_INVALID_STATE);

    assert(agent_peer_session_init(&session, &config));
    open = make_open();
    assert(agent_peer_session_begin(&session, 0U) == AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_transport_ready(&session, 1U) ==
           AGENT_PEER_SESSION_OK);
    open.version = 2U;
    assert(agent_peer_session_on_message(&session, &open, 2U) ==
           AGENT_PEER_SESSION_PROTOCOL_ERROR);
    assert(session.state == AGENT_PEER_SESSION_BACKOFF);
    assert(session.protocol_errors == 1U);
}

static void test_session_timeout_and_backoff(void)
{
    struct agent_peer_session_config config = make_session_config();
    struct agent_peer_session session;
    struct agent_arpx_message open = make_open();

    assert(agent_peer_session_init(&session, &config));
    assert(agent_peer_session_begin(&session, 100U) == AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_transport_ready(&session, 200U) ==
           AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_tick(&session, 5200U));
    assert(session.state == AGENT_PEER_SESSION_BACKOFF);
    assert(session.timeouts == 1U);
    assert(session.next_action_ms == 5450U);
    assert(!agent_peer_session_tick(&session, 5449U));
    assert(agent_peer_session_tick(&session, 5450U));
    assert(session.state == AGENT_PEER_SESSION_CONNECTING);
    assert(session.reconnects == 1U);

    assert(agent_peer_session_transport_ready(&session, 5500U) ==
           AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_on_message(&session, &open, 5600U) ==
           AGENT_PEER_SESSION_OK);
    assert(agent_peer_session_tick(&session, 11600U));
    assert(session.state == AGENT_PEER_SESSION_BACKOFF);
    assert(session.timeouts == 2U);
}

int main(void)
{
    test_open_round_trip();
    test_node_fixture_open_compatibility();
    test_heartbeat_round_trip();
    test_update_and_withdraw_round_trip();
    test_snapshot_round_trip();
    test_message_validation();
    test_strict_framing();
    test_session_open_and_heartbeat();
    test_session_identity_and_sequence_fail_closed();
    test_session_timeout_and_backoff();
    puts("ARPX protocol and peer session tests passed");
    return 0;
}

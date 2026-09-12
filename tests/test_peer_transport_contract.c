#include "agent_arpx_protocol.h"
#include "agent_peer_transport_contract.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct capture {
    size_t frames;
    struct agent_arpx_message messages[2];
};

static struct agent_arpx_message make_message(
    enum agent_arpx_message_type type,
    uint64_t sequence
)
{
    struct agent_arpx_message message;

    memset(&message, 0, sizeof(message));
    message.version = AGENT_ARPX_VERSION;
    message.type = type;
    snprintf(message.router_id, sizeof(message.router_id), "router-a");
    snprintf(message.domain_id, sizeof(message.domain_id), "eda.example");
    message.boot_epoch = 42U;
    message.sequence = sequence;
    message.heartbeat_ms = type == AGENT_ARPX_OPEN ? 2000U : 0U;
    return message;
}

static bool capture_frame(
    const uint8_t *frame,
    size_t frame_size,
    void *context
)
{
    struct capture *capture = context;

    if (capture->frames >= 2U ||
        agent_arpx_frame_decode(frame, frame_size,
                                &capture->messages[capture->frames]) !=
            AGENT_ARPX_OK) {
        return false;
    }
    capture->frames++;
    return true;
}

static void test_endpoint(void)
{
    struct agent_peer_transport_endpoint endpoint;

    assert(agent_peer_transport_ipv4_valid("192.168.250.2"));
    assert(agent_peer_transport_ipv4_valid("10.0.0.1"));
    assert(!agent_peer_transport_ipv4_valid("010.0.0.1"));
    assert(!agent_peer_transport_ipv4_valid("256.0.0.1"));
    assert(!agent_peer_transport_ipv4_valid("10.0.0"));
    assert(agent_peer_transport_endpoint_parse(
        "https://router-b.eda.example:7444/arpx/v1", "192.168.250.2",
        &endpoint));
    assert(strcmp(endpoint.server_identity, "router-b.eda.example") == 0);
    assert(strcmp(endpoint.authority, "router-b.eda.example:7444") == 0);
    assert(strcmp(endpoint.connect_ipv4, "192.168.250.2") == 0);
    assert(endpoint.port == 7444U);
    assert(!agent_peer_transport_endpoint_parse(
        "http://router-b.eda.example:7444/arpx/v1", "192.168.250.2",
        &endpoint));
    assert(!agent_peer_transport_endpoint_parse(
        "https://router-b.eda.example:7444/other", "192.168.250.2",
        &endpoint));
    assert(!agent_peer_transport_endpoint_parse(
        "https://router-b.eda.example:0/arpx/v1", "192.168.250.2",
        &endpoint));
}

static void test_deterministic_role(void)
{
    assert(agent_peer_transport_role_select("router-a", "router-b") ==
           AGENT_PEER_TRANSPORT_ROLE_DIAL);
    assert(agent_peer_transport_role_select("router-b", "router-a") ==
           AGENT_PEER_TRANSPORT_ROLE_ACCEPT);
    assert(agent_peer_transport_role_select("router-a", "router-a") ==
           AGENT_PEER_TRANSPORT_ROLE_INVALID);
    assert(agent_peer_transport_role_select("", "router-b") ==
           AGENT_PEER_TRANSPORT_ROLE_INVALID);
    assert(agent_peer_transport_role_select(NULL, "router-b") ==
           AGENT_PEER_TRANSPORT_ROLE_INVALID);
}

static void test_ingress(void)
{
    struct agent_peer_transport_ingress ingress;
    struct agent_arpx_message open = make_message(AGENT_ARPX_OPEN, 1U);
    struct agent_arpx_message heartbeat =
        make_message(AGENT_ARPX_HEARTBEAT, 2U);
    struct capture capture;
    uint8_t bytes[AGENT_ARPX_MAX_FRAME_SIZE * 2U];
    size_t first_size = 0U;
    size_t second_size = 0U;
    size_t emitted = 0U;

    memset(&capture, 0, sizeof(capture));
    assert(agent_arpx_frame_encode(&open, bytes, sizeof(bytes),
                                   &first_size) == AGENT_ARPX_OK);
    assert(agent_arpx_frame_encode(&heartbeat, bytes + first_size,
                                   sizeof(bytes) - first_size,
                                   &second_size) == AGENT_ARPX_OK);
    agent_peer_transport_ingress_init(&ingress);
    assert(agent_peer_transport_ingress_feed(
        &ingress, bytes, 2U, capture_frame, &capture, &emitted) ==
        AGENT_PEER_TRANSPORT_CONTRACT_OK);
    assert(emitted == 0U);
    assert(agent_peer_transport_ingress_feed(
        &ingress, bytes + 2U, first_size + second_size - 2U,
        capture_frame, &capture, &emitted) ==
        AGENT_PEER_TRANSPORT_CONTRACT_OK);
    assert(emitted == 2U);
    assert(capture.frames == 2U);
    assert(capture.messages[0].type == AGENT_ARPX_OPEN);
    assert(capture.messages[1].sequence == 2U);
}

static void test_invalid_frame_length(void)
{
    struct agent_peer_transport_ingress ingress;
    struct capture capture;
    const uint8_t invalid[] = { 0U, 0U, 0x10U, 0U };
    size_t emitted = 0U;

    memset(&capture, 0, sizeof(capture));
    agent_peer_transport_ingress_init(&ingress);
    assert(agent_peer_transport_ingress_feed(
        &ingress, invalid, sizeof(invalid), capture_frame, &capture,
        &emitted) == AGENT_PEER_TRANSPORT_CONTRACT_FRAME_TOO_LARGE);
    assert(ingress.used == 0U);
}

int main(void)
{
    test_endpoint();
    test_deterministic_role();
    test_ingress();
    test_invalid_frame_length();
    puts("peer transport contract tests passed");
    return 0;
}

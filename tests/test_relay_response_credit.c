/* Drive the production synchronous response sender against the real mux.
 * Only socket/IPC/event-loop boundaries are substituted. */
#include <assert.h>
#include "../feed/agentd/src/agent_relay_invoke.c"

static struct agent_relay_mux test_mux;
static uint8_t received[AGENT_INVOKE_MAX_RELAY_RESPONSE + 8192];
static size_t received_length;
static unsigned int end_count;

bool agent_peer_transport_tunnel_send(
    struct agent_peer_transport_manager *manager, const char *peer,
    const struct agent_relay_tunnel_message *message)
{
    (void)manager; (void)peer;
    if (agent_relay_mux_on_send(&test_mux, message) != AGENT_RELAY_TUNNEL_OK)
        return false;
    if (message->type == AGENT_RELAY_TUNNEL_DATA) {
        assert(received_length + message->data_length <= sizeof(received));
        memcpy(received + received_length, message->data, message->data_length);
        received_length += message->data_length;
    } else if (message->type == AGENT_RELAY_TUNNEL_END) end_count++;
    return true;
}
bool agent_peer_listener_tunnel_send(
    struct agent_peer_listener *manager, const char *peer,
    const struct agent_relay_tunnel_message *message)
{ (void)manager; (void)peer; (void)message; return false; }
#ifdef EXPECT_CREDIT_RESUME
bool agent_peer_transport_tunnel_send_credit(
    struct agent_peer_transport_manager *manager, const char *peer,
    uint32_t stream_id, uint32_t *credit)
{ (void)manager; (void)peer; return agent_relay_mux_send_credit(&test_mux, stream_id, credit); }
bool agent_peer_listener_tunnel_send_credit(
    struct agent_peer_listener *manager, const char *peer,
    uint32_t stream_id, uint32_t *credit)
{ (void)manager; (void)peer; (void)stream_id; (void)credit; return false; }
#endif
int uloop_timeout_cancel(struct uloop_timeout *timeout)
{ (void)timeout; return 0; }
int uloop_fd_delete(struct uloop_fd *fd)
{ (void)fd; return 0; }
int uloop_fd_add(struct uloop_fd *fd, unsigned int flags)
{ (void)fd; (void)flags; return 0; }
bool agent_ipc_client_respond_error(struct agent_ipc_client *client,
    uint32_t request_id, uint32_t code, const char *message)
{ (void)client; (void)request_id; (void)code; (void)message; return true; }

static void check_response(size_t body_size, bool streaming)
{
    struct agent_relay_invoke_manager manager = {0};
    struct relay_invoke_slot *slot = &manager.slots[0];
    struct agent_relay_tunnel_message msg = {0};
    static char expected[AGENT_INVOKE_MAX_RELAY_RESPONSE + 8192];
    int head;
    received_length = 0U;
    memset(received, 0, sizeof(received));
    end_count = 0U;
    assert(agent_relay_mux_init(&test_mux, true, 4U, 4096U));
    msg.type = AGENT_RELAY_TUNNEL_OPEN; msg.stream_id = 2U; msg.sequence = 1U;
    msg.streaming = streaming;
    msg.hop_limit = 8U; msg.max_latency_ms = 30000U;
    strcpy(msg.target_router_id, "router"); strcpy(msg.intent_class, "read");
    strcpy(msg.task_id, "credit-regression"); strcpy(msg.source_agent, "source");
    strcpy(msg.target_agent, "target"); strcpy(msg.tenant, "test"); strcpy(msg.region, "*");
    assert(agent_relay_mux_on_receive(&test_mux, &msg) == AGENT_RELAY_TUNNEL_OK);
    memset(&msg, 0, sizeof(msg));
    msg.type = AGENT_RELAY_TUNNEL_ACCEPT; msg.stream_id = 2U; msg.sequence = 1U;
    msg.status_code = 200U; msg.credit_bytes = 65536U;
    assert(agent_relay_mux_on_send(&test_mux, &msg) == AGENT_RELAY_TUNNEL_OK);
    msg.type = AGENT_RELAY_TUNNEL_END; msg.sequence = 2U;
    assert(agent_relay_mux_on_receive(&test_mux, &msg) == AGENT_RELAY_TUNNEL_OK);
    head = snprintf(expected, sizeof(expected),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", body_size);
    memset(expected + head, 'x', body_size);
    manager.stats.active = 1U;
    slot->manager = &manager; slot->role = RELAY_INVOKE_TARGET_READING;
    slot->backend_fd = -1; slot->stream_id = 2U; slot->next_send_sequence = 2U;
    slot->buffer_length = (size_t)head + body_size; slot->buffer_capacity = sizeof(expected);
    /* Reject oversized Content-Length immediately, before its body arrives. */
    if (body_size > AGENT_INVOKE_MAX_RELAY_RESPONSE) slot->buffer_length = (size_t)head;
    slot->buffer = malloc(slot->buffer_capacity); assert(slot->buffer != NULL);
    memcpy(slot->buffer, expected, slot->buffer_length);
    slot->streaming = streaming;
    if (streaming) {
        slot->stream_pending = malloc(slot->buffer_length);
        assert(slot->stream_pending != NULL);
        memcpy(slot->stream_pending, expected, slot->buffer_length);
        slot->stream_pending_length = slot->buffer_length;
        slot->stream_eof = true;
        assert(send_stream_start(slot));
        pump_target_stream(slot);
    } else finish_target(slot);
    if (body_size > AGENT_INVOKE_MAX_RELAY_RESPONSE) {
        assert(end_count == 1U && manager.stats.active == 0U);
        assert(strstr((const char *)received, "413 Payload Too Large") != NULL);
        assert(strstr((const char *)received, "RESPONSE_TOO_LARGE") != NULL);
        assert(strstr((const char *)received, "\"limit_bytes\":1048576") != NULL);
        assert(strstr((const char *)received, "\"response_bytes\":1048577") != NULL);
        return;
    }
    assert(manager.stats.failed == 0U && manager.stats.active == 1U);
    assert(received_length == 4096U && end_count == 0U);
#ifdef EXPECT_CREDIT_RESUME
    if (streaming) pump_target_stream(slot); else pump_target_response(slot); /* zero credit must be a harmless wait */
    assert(received_length == 4096U && test_mux.protocol_errors == 0U);
    memset(&msg, 0, sizeof(msg)); msg.type = AGENT_RELAY_TUNNEL_WINDOW_UPDATE;
    msg.stream_id = 2U; msg.sequence = 3U; msg.credit_bytes = 1000U;
    assert(agent_relay_mux_on_receive(&test_mux, &msg) == AGENT_RELAY_TUNNEL_OK);
    if (streaming) pump_target_stream(slot); else pump_target_response(slot);
    assert(received_length == 5096U && end_count == 0U && manager.stats.active == 1U);
    msg.sequence = 4U; msg.credit_bytes = 4096U;
    while (end_count == 0U) {
        assert(agent_relay_mux_on_receive(&test_mux, &msg) == AGENT_RELAY_TUNNEL_OK);
        if (streaming) pump_target_stream(slot); else pump_target_response(slot);
        msg.sequence++;
    }
    assert(received_length == (size_t)head + body_size && end_count == 1U);
    assert(memcmp(received, expected, received_length) == 0);
    assert(manager.stats.completed == 1U && manager.stats.failed == 0U);
    assert(manager.stats.active == 0U && test_mux.active_streams == 0U);
    assert(test_mux.protocol_errors == 0U);
#endif
}

int main(void)
{
    check_response(9000U, false);
    check_response(16384U, false);
    check_response(16385U, false);
    check_response(65537U, false);
    check_response(AGENT_INVOKE_MAX_RELAY_RESPONSE, false);
    check_response(AGENT_INVOKE_MAX_RELAY_RESPONSE + 1U, false);
    check_response(200000U, true);
    puts("production Relay response credit and oversize regression passed");
    return 0;
}

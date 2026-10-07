#include "agent_relay_invoke.h"

#include "agent_invoke_contract.h"
#include "agent_peer_listener.h"
#include "agent_replay_cache.h"
#include "agent_sse_contract.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <libubox/uloop.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RELAY_INVOKE_MAX_TRANSACTIONS 32U
#define RELAY_INVOKE_MAX_WIRE_RESPONSE \
    (AGENT_INVOKE_MAX_HEADER_BYTES + AGENT_IPC_MAX_INVOKE_BODY)
#define RELAY_INVOKE_REQUEST_RESERVE 2048U
#define RELAY_INVOKE_INITIAL_CREDIT 65536U
#define RELAY_INVOKE_CREDIT_BATCH 32768U
#define RELAY_STREAM_MAX_EVENT 32768U
#define RELAY_STREAM_MAX_BYTES AGENT_IPC_MAX_STREAM_BYTES
#define RELAY_INVOKE_RESET_UNAVAILABLE 1U
#define RELAY_INVOKE_RESET_PROTOCOL 3U
#define RELAY_INVOKE_RESET_BACKPRESSURE 4U
#define RELAY_INVOKE_RESET_AUTHORIZATION 6U

enum relay_invoke_role {
    RELAY_INVOKE_FREE = 0,
    RELAY_INVOKE_SOURCE,
    RELAY_INVOKE_TARGET_RECEIVING,
    RELAY_INVOKE_TARGET_CONNECTING,
    RELAY_INVOKE_TARGET_SENDING,
    RELAY_INVOKE_TARGET_READING,
    RELAY_INVOKE_TARGET_RESPONDING,
    RELAY_INVOKE_TRANSIT_OPENING,
    RELAY_INVOKE_TRANSIT_FORWARDING
};

enum relay_invoke_leg {
    RELAY_INVOKE_UPSTREAM = 0,
    RELAY_INVOKE_DOWNSTREAM
};

struct relay_invoke_slot {
    enum relay_invoke_role role;
    struct agent_relay_invoke_manager *manager;
    struct agent_ipc_client *client;
    uint32_t request_id;
    uint32_t stream_id;
    uint64_t next_send_sequence;
    bool streaming;
    bool response_started;
    bool internal_gateway;
    bool mesh_direct;
    char peer_id[AGENT_IPC_PEER_ID_LEN];
    char downstream_peer_id[AGENT_IPC_PEER_ID_LEN];
    uint32_t downstream_stream_id;
    uint64_t downstream_next_send_sequence;
    bool upstream_ended;
    bool downstream_ended;
    struct agent_relay_tunnel_message open;
    bool forwarding_verified;
    struct agent_forwarding_verified forwarding;
    uint8_t *buffer;
    size_t buffer_length;
    size_t buffer_capacity;
    size_t response_sent;
    int backend_fd;
    struct uloop_fd backend;
    struct uloop_timeout timeout;
    uint32_t timeout_ms;
    uint64_t deadline_ms;
    char *backend_request;
    size_t backend_request_length;
    size_t backend_request_sent;
    char *stream_event_buffer;
    struct agent_sse_relay stream_relay;
    uint64_t stream_bytes;
    uint32_t pending_credit;
};

struct agent_relay_invoke_manager {
    struct route_table *routes;
    struct peer_table *peers;
    struct agent_peer_transport_manager *transport;
    struct agent_peer_listener *listener;
    uint64_t (*now_ms)(void);
    char local_router_id[AGENT_IPC_ROUTER_ID_LEN];
    bool forwarding_required;
    bool open_mesh;
    struct agent_forwarding_verifier forwarding_verifier;
    char forwarding_source_router_id[AGENT_IPC_ROUTER_ID_LEN];
    struct agent_replay_cache forwarding_replay;
    bool internal_gateway_enabled;
    struct agent_invoke_endpoint internal_gateway_endpoint;
    char internal_gateway_token[AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U];
    agent_relay_tenant_alias_resolver tenant_alias_resolver;
    void *tenant_alias_context;
    struct relay_invoke_slot slots[RELAY_INVOKE_MAX_TRANSACTIONS];
    struct agent_relay_invoke_stats stats;
};

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL) return false;
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static void erase_secret(void *value, size_t length)
{
    volatile unsigned char *cursor = value;

    while (cursor != NULL && length > 0U) {
        *cursor++ = 0U;
        length--;
    }
}

static bool internal_token_file_is_safe(const char *path)
{
    return path != NULL &&
           strcmp(path, "/etc/agentd/nexus-cloud-gateway.token") == 0;
}

static bool load_internal_gateway_token(
    const char *path,
    char token[AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U]
)
{
    struct stat metadata;
    size_t used = 0U;
    ssize_t result;
    char extra;
    int descriptor;

    if (!internal_token_file_is_safe(path)) return false;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0777U) != 0600U) {
        if (descriptor >= 0) close(descriptor);
        return false;
    }
    while (used < AGENT_INVOKE_INTERNAL_TOKEN_LEN) {
        result = read(descriptor, token + used,
                      AGENT_INVOKE_INTERNAL_TOKEN_LEN - used);
        if (result > 0) {
            used += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        close(descriptor);
        return false;
    }
    do {
        result = read(descriptor, &extra, 1U);
    } while (result < 0 && errno == EINTR);
    if (result != 0 || close(descriptor) != 0 ||
        !agent_invoke_internal_token_is_valid(token, used)) {
        erase_secret(token, AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U);
        return false;
    }
    token[used] = '\0';
    return true;
}

static uint64_t wall_clock_seconds(void)
{
    time_t now = time(NULL);

    return now < 0 ? 0U : (uint64_t)now;
}

static bool verify_forwarding_open(
    struct agent_relay_invoke_manager *manager,
    const struct agent_relay_tunnel_message *open,
    struct agent_forwarding_verified *verified,
    bool mesh_direct
)
{
    struct agent_forwarding_context expected;
    enum agent_forwarding_result result;
    enum agent_replay_result replay_result;
    uint64_t now = wall_clock_seconds();
    uint64_t replay_expires;

    if (!manager->forwarding_required || mesh_direct) return true;
    if (open->forwarding_assertion[0] == '\0' || now == 0U) {
        manager->stats.forwarding_assertions_rejected++;
        return false;
    }
    memset(&expected, 0, sizeof(expected));
    expected.source_router_id = manager->forwarding_source_router_id;
    expected.target_router_id = open->target_router_id;
    expected.source_agent = open->source_agent;
    expected.tenant = open->tenant;
    expected.intent = open->intent_class;
    expected.task_id = open->task_id;
    expected.hop_limit = open->hop_limit;
    result = agent_forwarding_verify(
        &manager->forwarding_verifier, open->forwarding_assertion,
        &expected, now, verified);
    if (result != AGENT_FORWARDING_OK) {
        manager->stats.forwarding_assertions_rejected++;
        return false;
    }
    replay_expires = verified->expires_at;
    if (replay_expires <= UINT64_MAX -
            manager->forwarding_verifier.clock_skew_seconds - 1U) {
        replay_expires +=
            manager->forwarding_verifier.clock_skew_seconds + 1U;
    } else {
        replay_expires = UINT64_MAX;
    }
    replay_result = agent_replay_cache_check_and_store(
        &manager->forwarding_replay, verified->issuer, verified->nonce,
        replay_expires, now);
    if (replay_result != AGENT_REPLAY_ACCEPTED) {
        if (replay_result == AGENT_REPLAY_DETECTED) {
            manager->stats.forwarding_assertion_replays++;
        }
        manager->stats.forwarding_assertions_rejected++;
        return false;
    }
    manager->stats.forwarding_assertions_verified++;
    return true;
}

static void clear_slot(struct relay_invoke_slot *slot)
{
    struct agent_relay_invoke_manager *manager;

    if (slot == NULL || slot->role == RELAY_INVOKE_FREE) return;
    manager = slot->manager;
    uloop_timeout_cancel(&slot->timeout);
    if (slot->backend_fd >= 0) {
        uloop_fd_delete(&slot->backend);
        close(slot->backend_fd);
    }
    free(slot->buffer);
    if (slot->internal_gateway && slot->backend_request != NULL) {
        erase_secret(slot->backend_request, slot->backend_request_length);
    }
    free(slot->backend_request);
    free(slot->stream_event_buffer);
    memset(slot, 0, sizeof(*slot));
    slot->backend_fd = -1;
    slot->manager = manager;
    if (manager->stats.active > 0U) manager->stats.active--;
}

static struct relay_invoke_slot *allocate_slot(
    struct agent_relay_invoke_manager *manager
)
{
    size_t index;

    for (index = 0U; index < RELAY_INVOKE_MAX_TRANSACTIONS; index++) {
        if (manager->slots[index].role == RELAY_INVOKE_FREE) {
            manager->slots[index].manager = manager;
            manager->slots[index].backend_fd = -1;
            manager->stats.active++;
            return &manager->slots[index];
        }
    }
    return NULL;
}

static struct relay_invoke_slot *find_stream(
    struct agent_relay_invoke_manager *manager,
    const char *peer_id,
    uint32_t stream_id,
    enum relay_invoke_leg *leg
)
{
    size_t index;

    for (index = 0U; index < RELAY_INVOKE_MAX_TRANSACTIONS; index++) {
        struct relay_invoke_slot *slot = &manager->slots[index];

        if (slot->role != RELAY_INVOKE_FREE &&
            slot->stream_id == stream_id &&
            strcmp(slot->peer_id, peer_id) == 0) {
            if (leg != NULL) *leg = RELAY_INVOKE_UPSTREAM;
            return slot;
        }
        if ((slot->role == RELAY_INVOKE_TRANSIT_OPENING ||
             slot->role == RELAY_INVOKE_TRANSIT_FORWARDING) &&
            slot->downstream_stream_id == stream_id &&
            strcmp(slot->downstream_peer_id, peer_id) == 0) {
            if (leg != NULL) *leg = RELAY_INVOKE_DOWNSTREAM;
            return slot;
        }
    }
    return NULL;
}

static bool send_tunnel_leg(
    struct relay_invoke_slot *slot,
    enum relay_invoke_leg leg,
    struct agent_relay_tunnel_message *message
)
{
    const char *peer_id = leg == RELAY_INVOKE_DOWNSTREAM
        ? slot->downstream_peer_id : slot->peer_id;
    uint32_t stream_id = leg == RELAY_INVOKE_DOWNSTREAM
        ? slot->downstream_stream_id : slot->stream_id;
    uint64_t *next_sequence = leg == RELAY_INVOKE_DOWNSTREAM
        ? &slot->downstream_next_send_sequence : &slot->next_send_sequence;

    message->stream_id = stream_id;
    message->sequence = *next_sequence;
    if (!agent_peer_transport_tunnel_send(
            slot->manager->transport, peer_id, message) &&
        !agent_peer_listener_tunnel_send(
            slot->manager->listener, peer_id, message)) {
        return false;
    }
    (*next_sequence)++;
    return true;
}

static bool send_tunnel(
    struct relay_invoke_slot *slot,
    struct agent_relay_tunnel_message *message
)
{
    return send_tunnel_leg(slot, RELAY_INVOKE_UPSTREAM, message);
}

static void send_reset(struct relay_invoke_slot *slot, uint16_t code)
{
    struct agent_relay_tunnel_message message;

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_RESET;
    message.reset_code = code;
    (void)send_tunnel(slot, &message);
}

static void send_reset_leg(
    struct relay_invoke_slot *slot,
    enum relay_invoke_leg leg,
    uint16_t code
)
{
    struct agent_relay_tunnel_message message;

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_RESET;
    message.reset_code = code;
    (void)send_tunnel_leg(slot, leg, &message);
}

static bool send_window_update(
    struct relay_invoke_slot *slot,
    uint32_t credit_bytes
)
{
    struct agent_relay_tunnel_message message;

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_WINDOW_UPDATE;
    message.credit_bytes = credit_bytes;
    return send_tunnel(slot, &message);
}

static void fail_slot(
    struct relay_invoke_slot *slot,
    uint32_t ipc_code,
    const char *message,
    uint16_t reset_code
)
{
    bool transit = slot->role == RELAY_INVOKE_TRANSIT_OPENING ||
                   slot->role == RELAY_INVOKE_TRANSIT_FORWARDING;

    if (slot->role == RELAY_INVOKE_SOURCE && slot->client != NULL) {
        (void)agent_ipc_client_respond_error(
            slot->client, slot->request_id, ipc_code, message);
        slot->client = NULL;
    }
    if (slot->stream_id != 0U) send_reset(slot, reset_code);
    if (transit && slot->downstream_stream_id != 0U) {
        send_reset_leg(slot, RELAY_INVOKE_DOWNSTREAM, reset_code);
        slot->manager->stats.transit_failed++;
    }
    if (slot->internal_gateway) {
        slot->manager->stats.internal_gateway_failed++;
    }
    slot->manager->stats.failed++;
    clear_slot(slot);
}

static bool append_buffer(
    struct relay_invoke_slot *slot,
    const uint8_t *data,
    size_t length
)
{
    if (length > slot->buffer_capacity - slot->buffer_length) return false;
    memcpy(slot->buffer + slot->buffer_length, data, length);
    slot->buffer_length += length;
    slot->manager->stats.bytes_received += length;
    return true;
}

static bool send_data_bytes(
    struct relay_invoke_slot *slot,
    const uint8_t *data,
    size_t length
)
{
    struct agent_relay_tunnel_message message;
    size_t offset = 0U;
    size_t chunk;

    while (offset < length) {
        chunk = length - offset;
        if (chunk > AGENT_RELAY_TUNNEL_MAX_DATA) {
            chunk = AGENT_RELAY_TUNNEL_MAX_DATA;
        }
        memset(&message, 0, sizeof(message));
        message.type = AGENT_RELAY_TUNNEL_DATA;
        memcpy(message.data, data + offset, chunk);
        message.data_length = chunk;
        if (!send_tunnel(slot, &message)) return false;
        slot->manager->stats.bytes_sent += chunk;
        offset += chunk;
    }
    return true;
}

static bool send_bytes(struct relay_invoke_slot *slot)
{
    struct agent_relay_tunnel_message message;

    if (!send_data_bytes(slot, slot->buffer, slot->buffer_length)) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_END;
    return send_tunnel(slot, &message);
}

static void timeout_callback(struct uloop_timeout *timeout)
{
    struct relay_invoke_slot *slot =
        container_of(timeout, struct relay_invoke_slot, timeout);

    slot->manager->stats.timed_out++;
    fail_slot(slot, 504U, "Relay invoke deadline expired",
              RELAY_INVOKE_RESET_UNAVAILABLE);
}

static bool set_timeout(struct relay_invoke_slot *slot, uint32_t timeout_ms)
{
    if (timeout_ms < 10U) timeout_ms = 10U;
    if (timeout_ms > 30000U) timeout_ms = 30000U;
    slot->timeout_ms = timeout_ms;
    slot->deadline_ms = slot->manager->now_ms() + timeout_ms;
    slot->timeout.cb = timeout_callback;
    return uloop_timeout_set(&slot->timeout, (int)timeout_ms) == 0;
}

static void refresh_timeout(struct relay_invoke_slot *slot)
{
    slot->deadline_ms = slot->manager->now_ms() + slot->timeout_ms;
    (void)uloop_timeout_set(&slot->timeout, (int)slot->timeout_ms);
}

static void finish_source(struct relay_invoke_slot *slot)
{
    struct agent_invoke_http_response parsed;
    struct agent_ipc_invoke_response response;
    enum agent_invoke_http_result result;

    result = agent_invoke_parse_http_response(
        (const char *)slot->buffer, slot->buffer_length,
        AGENT_IPC_MAX_INVOKE_BODY, &parsed);
    if (result != AGENT_INVOKE_HTTP_OK) {
        fail_slot(slot, 502U, "invalid Relay target response",
                  RELAY_INVOKE_RESET_PROTOCOL);
        return;
    }
    memset(&response, 0, sizeof(response));
    response.request_id = slot->request_id;
    response.status_code = (uint16_t)parsed.status;
    if (!copy_text(response.content_type, sizeof(response.content_type),
                   parsed.content_type)) {
        fail_slot(slot, 502U, "Relay response content type is invalid",
                  RELAY_INVOKE_RESET_PROTOCOL);
        return;
    }
    response.body_length = parsed.body_length;
    memcpy(response.body, slot->buffer + parsed.header_length,
           parsed.body_length);
    if (!agent_ipc_client_respond_invoke(slot->client, &response)) {
        slot->manager->stats.failed++;
    } else {
        slot->manager->stats.completed++;
    }
    slot->client = NULL;
    clear_slot(slot);
}

static void backend_events(struct relay_invoke_slot *slot, unsigned int events)
{
    uloop_fd_delete(&slot->backend);
    (void)uloop_fd_add(&slot->backend, events);
}

static void pump_target_response(struct relay_invoke_slot *slot)
{
    struct agent_relay_tunnel_message message;
    uint32_t credit;
    while (slot->response_sent < slot->buffer_length) {
        size_t chunk = slot->buffer_length - slot->response_sent;
        if (!agent_peer_transport_tunnel_send_credit(
                slot->manager->transport, slot->peer_id, slot->stream_id, &credit) &&
            !agent_peer_listener_tunnel_send_credit(
                slot->manager->listener, slot->peer_id, slot->stream_id, &credit)) {
            fail_slot(slot, 502U, "Relay response stream unavailable",
                      RELAY_INVOKE_RESET_UNAVAILABLE);
            return;
        }
        /* Retain the bounded response and original deadline until the peer
         * grants more credit; lack of credit is not a protocol error. */
        if (credit == 0U) return;
        if (chunk > AGENT_RELAY_TUNNEL_MAX_DATA) chunk = AGENT_RELAY_TUNNEL_MAX_DATA;
        if (chunk > credit) chunk = credit;
        memset(&message, 0, sizeof(message));
        message.type = AGENT_RELAY_TUNNEL_DATA;
        message.data_length = chunk;
        memcpy(message.data, slot->buffer + slot->response_sent, chunk);
        if (!send_tunnel(slot, &message)) {
            fail_slot(slot, 503U, "Relay response queue is full",
                      RELAY_INVOKE_RESET_BACKPRESSURE);
            return;
        }
        slot->response_sent += chunk;
        slot->manager->stats.bytes_sent += chunk;
    }
    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_END;
    if (!send_tunnel(slot, &message)) {
        fail_slot(slot, 503U, "Relay response end queue is full",
                  RELAY_INVOKE_RESET_BACKPRESSURE);
        return;
    }
    if (slot->internal_gateway) slot->manager->stats.internal_gateway_completed++;
    slot->manager->stats.completed++;
    clear_slot(slot);
}

static void finish_target(struct relay_invoke_slot *slot)
{
    struct agent_invoke_http_response parsed;
    enum agent_invoke_http_result result;

    result = agent_invoke_parse_http_response(
        (const char *)slot->buffer, slot->buffer_length,
        AGENT_IPC_MAX_INVOKE_BODY, &parsed);
    if (result != AGENT_INVOKE_HTTP_OK) {
        fail_slot(slot, 502U, "local Agent returned an invalid response",
                  RELAY_INVOKE_RESET_PROTOCOL);
        return;
    }
    if (slot->backend_fd >= 0) {
        uloop_fd_delete(&slot->backend);
        close(slot->backend_fd);
        slot->backend_fd = -1;
    }
    slot->role = RELAY_INVOKE_TARGET_RESPONDING;
    slot->response_sent = 0U;
    pump_target_response(slot);
}

static bool send_stream_start(struct relay_invoke_slot *slot)
{
    struct agent_relay_tunnel_message message;

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_RESPONSE_START;
    message.status_code = 200U;
    return send_tunnel(slot, &message);
}

static bool emit_target_stream_event(
    const char *event,
    size_t event_length,
    void *context
)
{
    struct relay_invoke_slot *slot = context;

    if (event_length > RELAY_STREAM_MAX_BYTES - slot->stream_bytes ||
        !send_data_bytes(slot, (const uint8_t *)event, event_length)) {
        slot->manager->stats.stream_backpressure_resets++;
        return false;
    }
    slot->stream_bytes += event_length;
    slot->manager->stats.stream_events++;
    slot->manager->stats.stream_bytes += event_length;
    refresh_timeout(slot);
    return true;
}

static void complete_target_stream(struct relay_invoke_slot *slot)
{
    struct agent_relay_tunnel_message message;

    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_END;
    if (!send_tunnel(slot, &message)) {
        fail_slot(slot, 503U, "Relay stream queue is full",
                  RELAY_INVOKE_RESET_BACKPRESSURE);
        return;
    }
    slot->manager->stats.completed++;
    slot->manager->stats.streams_completed++;
    clear_slot(slot);
}

static void handle_target_stream_read(struct relay_invoke_slot *slot)
{
    struct agent_sse_response_head head;
    enum agent_sse_head_result head_result;
    enum agent_sse_relay_result relay_result;
    char *destination;
    size_t capacity;
    ssize_t received;

    if (slot->response_started) {
        destination = (char *)slot->buffer;
        capacity = slot->buffer_capacity - 1U;
    } else {
        if (slot->buffer_length + 1U >= slot->buffer_capacity) {
            fail_slot(slot, 502U, "local SSE headers exceed the bound",
                      RELAY_INVOKE_RESET_PROTOCOL);
            return;
        }
        destination = (char *)slot->buffer + slot->buffer_length;
        capacity = slot->buffer_capacity - slot->buffer_length - 1U;
    }
    received = recv(slot->backend_fd, destination, capacity, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                         errno == EINTR)) return;
    if (received < 0) {
        fail_slot(slot, 502U, "local SSE read failed",
                  RELAY_INVOKE_RESET_UNAVAILABLE);
        return;
    }
    if (received > 0) {
        slot->manager->stats.bytes_received += (size_t)received;
        refresh_timeout(slot);
    }
    if (!slot->response_started) {
        slot->buffer_length += (size_t)received;
        slot->buffer[slot->buffer_length] = '\0';
        head_result = agent_sse_parse_response_head(
            (const char *)slot->buffer, slot->buffer_length, &head);
        if (head_result == AGENT_SSE_HEAD_INCOMPLETE && received > 0) return;
        if (head_result != AGENT_SSE_HEAD_OK ||
            !agent_sse_response_is_streamable(&head) ||
            !send_stream_start(slot)) {
            fail_slot(slot, 502U, "local Agent did not return bounded SSE",
                      RELAY_INVOKE_RESET_PROTOCOL);
            return;
        }
        agent_sse_relay_init(&slot->stream_relay, head.chunked,
                             slot->stream_event_buffer,
                             RELAY_STREAM_MAX_EVENT);
        slot->response_started = true;
        destination = (char *)slot->buffer + head.header_length;
        capacity = slot->buffer_length - head.header_length;
    } else {
        capacity = (size_t)received;
    }
    if (capacity > 0U) {
        relay_result = agent_sse_relay_feed(
            &slot->stream_relay, destination, capacity,
            emit_target_stream_event, slot);
        if (relay_result == AGENT_SSE_RELAY_DONE) {
            complete_target_stream(slot);
            return;
        }
        if (relay_result != AGENT_SSE_RELAY_OK) {
            fail_slot(slot, 502U,
                      relay_result == AGENT_SSE_RELAY_TOO_LARGE
                          ? "local SSE event exceeds the bound"
                          : "local SSE framing is invalid",
                      relay_result == AGENT_SSE_RELAY_EMIT_FAILED
                          ? RELAY_INVOKE_RESET_BACKPRESSURE
                          : RELAY_INVOKE_RESET_PROTOCOL);
            return;
        }
    }
    slot->buffer_length = 0U;
    if (received == 0) {
        relay_result = agent_sse_relay_finish(&slot->stream_relay);
        if (relay_result == AGENT_SSE_RELAY_DONE) {
            complete_target_stream(slot);
        } else {
            fail_slot(slot, 502U, "local SSE ended inside an event",
                      RELAY_INVOKE_RESET_PROTOCOL);
        }
    }
}

static void backend_callback(struct uloop_fd *fd, unsigned int events)
{
    struct relay_invoke_slot *slot =
        container_of(fd, struct relay_invoke_slot, backend);
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    ssize_t result;

    (void)events;
    if (slot->role == RELAY_INVOKE_TARGET_CONNECTING) {
        if (getsockopt(slot->backend_fd, SOL_SOCKET, SO_ERROR,
                       &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            fail_slot(slot, 502U, "local Agent connect failed",
                      RELAY_INVOKE_RESET_UNAVAILABLE);
            return;
        }
        slot->role = RELAY_INVOKE_TARGET_SENDING;
    }
    if (slot->role == RELAY_INVOKE_TARGET_SENDING) {
        result = send(slot->backend_fd,
                      slot->backend_request + slot->backend_request_sent,
                      slot->backend_request_length -
                          slot->backend_request_sent,
                      MSG_NOSIGNAL);
        if (result > 0) {
            slot->backend_request_sent += (size_t)result;
            if (slot->backend_request_sent == slot->backend_request_length) {
                slot->role = RELAY_INVOKE_TARGET_READING;
                backend_events(slot, ULOOP_READ);
            }
            return;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                           errno == EINTR)) return;
        fail_slot(slot, 502U, "local Agent write failed",
                  RELAY_INVOKE_RESET_UNAVAILABLE);
        return;
    }
    if (slot->role != RELAY_INVOKE_TARGET_READING) return;
    if (slot->streaming) {
        handle_target_stream_read(slot);
        return;
    }
    result = recv(slot->backend_fd, slot->buffer + slot->buffer_length,
                  slot->buffer_capacity - slot->buffer_length, 0);
    if (result > 0) {
        struct agent_invoke_http_response parsed;
        enum agent_invoke_http_result parsed_result;

        slot->buffer_length += (size_t)result;
        slot->manager->stats.bytes_received += (size_t)result;
        parsed_result = agent_invoke_parse_http_response(
            (const char *)slot->buffer, slot->buffer_length,
            AGENT_IPC_MAX_INVOKE_BODY, &parsed);
        if (parsed_result == AGENT_INVOKE_HTTP_OK) finish_target(slot);
        else if (parsed_result != AGENT_INVOKE_HTTP_INCOMPLETE) {
            fail_slot(slot, 502U, "local Agent response is invalid",
                      RELAY_INVOKE_RESET_PROTOCOL);
        }
        return;
    }
    if (result == 0) {
        finish_target(slot);
        return;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        fail_slot(slot, 502U, "local Agent read failed",
                  RELAY_INVOKE_RESET_UNAVAILABLE);
    }
}

static bool start_local_invoke(struct relay_invoke_slot *slot)
{
    struct route_query query;
    struct route_selection selection;
    struct agent_invoke_endpoint endpoint;
    struct agent_invoke_endpoint selected_tls_endpoint;
    struct sockaddr_in address;
    size_t request_capacity;
    bool selected_https;
    uint64_t now;
    uint64_t remaining;
    uint32_t gateway_timeout_ms = 0U;
    char local_tenant[AGENT_IPC_TENANT_LEN] = {0};
    const char *lookup_tenant = slot->open.tenant;
    int connected;

    if (slot->mesh_direct) {
        const struct agent_route *route;
        bool matched = false;

        for (route = route_table_first(slot->manager->routes);
             route != NULL; route = route->next) {
            if ((route->source != AGENT_ROUTE_SOURCE_LOCAL &&
                 route->source != AGENT_ROUTE_SOURCE_STATIC) ||
                strcmp(route->intent, slot->open.intent_class) != 0 ||
                route->version != 1U || !route->healthy ||
                route->lease_expires_ms <= slot->manager->now_ms() ||
                (slot->open.target_agent[0] != '\0' &&
                 strcmp(route->origin, slot->open.target_agent) != 0)) {
                continue;
            }
            if (matched && strcmp(local_tenant, route->tenant) != 0) {
                return false;
            }
            if (!copy_text(local_tenant, sizeof(local_tenant),
                           route->tenant)) return false;
            matched = true;
        }
        if (!matched) return false;
        lookup_tenant = local_tenant;
    } else if (slot->manager->forwarding_required &&
        slot->manager->tenant_alias_resolver != NULL) {
        if (slot->manager->tenant_alias_resolver(
                slot->open.tenant, slot->open.target_agent,
                slot->open.intent_class, local_tenant,
                slot->manager->tenant_alias_context)) {
            lookup_tenant = local_tenant;
            slot->manager->stats.cloud_tenant_aliases++;
        } else {
            slot->manager->stats.cloud_tenant_alias_rejections++;
        }
    }
    memset(&query, 0, sizeof(query));
    if (!copy_text(query.intent, sizeof(query.intent),
                   slot->open.intent_class) ||
        !copy_text(query.tenant, sizeof(query.tenant), lookup_tenant) ||
        (slot->open.target_agent[0] != '\0' &&
         !copy_text(query.target_agent, sizeof(query.target_agent),
                    slot->open.target_agent)) ||
        (strcmp(slot->open.region, "*") != 0 &&
         !copy_text(query.region, sizeof(query.region), slot->open.region))) {
        return false;
    }
    query.version = 1U;
    query.max_cost_microunits = slot->open.max_cost_microunits;
    query.max_latency_ms = slot->open.max_latency_ms;
    if (!route_table_lookup_local(slot->manager->routes, &query,
                                  slot->manager->now_ms(), &selection)) {
        return false;
    }
    selected_https = agent_invoke_parse_remote_tls_endpoint(
        selection.route->endpoint, &selected_tls_endpoint);
    if (selected_https) {
        now = slot->manager->now_ms();
        if (slot->streaming || slot->open.target_agent[0] == '\0' ||
            now >= slot->deadline_ms ||
            !slot->manager->internal_gateway_enabled) {
            slot->manager->stats.internal_gateway_unavailable++;
            return false;
        }
        remaining = slot->deadline_ms - now;
        if (remaining > 30000U) remaining = 30000U;
        if (remaining < 10U) {
            slot->manager->stats.internal_gateway_unavailable++;
            return false;
        }
        gateway_timeout_ms = (uint32_t)remaining;
        endpoint = slot->manager->internal_gateway_endpoint;
        slot->internal_gateway = true;
    } else if ((!agent_invoke_parse_loopback_endpoint(
                    selection.route->endpoint, &endpoint) &&
                !agent_invoke_parse_lan_endpoint(
                    selection.route->endpoint, &endpoint)) ||
               endpoint.family != AGENT_INVOKE_IPV4) {
        return false;
    }
    request_capacity = slot->buffer_length +
                       RELAY_INVOKE_REQUEST_RESERVE + 1U;
    slot->backend_request = malloc(request_capacity);
    if (slot->backend_request == NULL ||
        !(selected_https
              ? agent_invoke_build_internal_http_request(
                    &endpoint, selection.route->route_id,
                    selection.route->endpoint, selection.route->origin,
                    slot->open.target_agent,
                    slot->manager->internal_gateway_token,
                    gateway_timeout_ms,
                    (const char *)slot->buffer, slot->buffer_length,
                    slot->backend_request, request_capacity,
                    &slot->backend_request_length)
              : slot->streaming
                  ? agent_invoke_build_stream_http_request(
                        &endpoint, selection.route->route_id,
                        (const char *)slot->buffer, slot->buffer_length,
                        slot->backend_request, request_capacity,
                        &slot->backend_request_length)
                  : agent_invoke_build_http_request(
                        &endpoint, selection.route->route_id,
                        (const char *)slot->buffer, slot->buffer_length,
                        slot->backend_request, request_capacity,
                        &slot->backend_request_length))) return false;
    if (selected_https) {
        slot->manager->stats.internal_gateway_started++;
    }
    free(slot->buffer);
    slot->buffer_capacity = slot->streaming
        ? AGENT_SSE_MAX_HEADER_BYTES + 1U
        : RELAY_INVOKE_MAX_WIRE_RESPONSE;
    slot->buffer = malloc(slot->buffer_capacity);
    slot->buffer_length = 0U;
    if (slot->streaming) {
        slot->stream_event_buffer = malloc(RELAY_STREAM_MAX_EVENT);
    }
    if (slot->buffer == NULL ||
        (slot->streaming && slot->stream_event_buffer == NULL)) return false;
    slot->backend_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK |
                              SOCK_CLOEXEC, 0);
    if (slot->backend_fd < 0) return false;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.host, &address.sin_addr) != 1) return false;
    slot->backend.fd = slot->backend_fd;
    slot->backend.cb = backend_callback;
    connected = connect(slot->backend_fd, (struct sockaddr *)&address,
                        sizeof(address));
    if (connected == 0) slot->role = RELAY_INVOKE_TARGET_SENDING;
    else if (errno == EINPROGRESS || errno == EAGAIN ||
             errno == EWOULDBLOCK) {
        slot->role = RELAY_INVOKE_TARGET_CONNECTING;
    } else return false;
    return uloop_fd_add(&slot->backend, ULOOP_WRITE) == 0;
}

static bool signed_envelope_target_matches(
    const struct relay_invoke_slot *slot
)
{
    struct json_tokener *tokener = NULL;
    struct json_object *root = NULL;
    struct json_object *target;
    bool matches = false;

    if (slot == NULL || slot->open.target_agent[0] == '\0' ||
        slot->buffer == NULL || slot->buffer_length == 0U ||
        slot->buffer_length > INT_MAX) return false;
    tokener = json_tokener_new_ex(32);
    if (tokener == NULL) return false;
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(
        tokener, (const char *)slot->buffer, (int)slot->buffer_length);
    matches = root != NULL &&
        json_tokener_get_error(tokener) == json_tokener_success &&
        json_tokener_get_parse_end(tokener) == slot->buffer_length &&
        json_object_is_type(root, json_type_object) &&
        json_object_object_get_ex(root, "target_agent", &target) &&
        json_object_is_type(target, json_type_string) &&
        strcmp(json_object_get_string(target), slot->open.target_agent) == 0;
    if (root != NULL) json_object_put(root);
    json_tokener_free(tokener);
    return matches;
}

static bool send_open_reset(
    struct agent_relay_invoke_manager *manager,
    const char *peer_id,
    uint32_t stream_id,
    uint16_t reset_code
)
{
    struct agent_relay_tunnel_message reset;

    memset(&reset, 0, sizeof(reset));
    reset.type = AGENT_RELAY_TUNNEL_RESET;
    reset.stream_id = stream_id;
    reset.sequence = 1U;
    reset.reset_code = reset_code;
    return agent_peer_transport_tunnel_send(
               manager->transport, peer_id, &reset) ||
           agent_peer_listener_tunnel_send(
               manager->listener, peer_id, &reset);
}

static bool start_transit_invoke(
    struct agent_relay_invoke_manager *manager,
    const char *upstream_peer_id,
    const struct agent_relay_tunnel_message *open,
    uint64_t now_ms
)
{
    struct route_query query;
    struct route_selection selection;
    struct relay_invoke_slot *slot;
    struct agent_relay_tunnel_message downstream_open;

    if (open->hop_limit <= 1U) {
        manager->stats.transit_hop_limit_rejected++;
        manager->stats.transit_failed++;
        manager->stats.failed++;
        return send_open_reset(manager, upstream_peer_id, open->stream_id,
                               RELAY_INVOKE_RESET_UNAVAILABLE);
    }
    memset(&query, 0, sizeof(query));
    if (!copy_text(query.intent, sizeof(query.intent), open->intent_class) ||
        !copy_text(query.tenant, sizeof(query.tenant), open->tenant) ||
        (open->target_agent[0] != '\0' &&
         !copy_text(query.target_agent, sizeof(query.target_agent),
                    open->target_agent)) ||
        (strcmp(open->region, "*") != 0 &&
         !copy_text(query.region, sizeof(query.region), open->region))) {
        return send_open_reset(manager, upstream_peer_id, open->stream_id,
                               RELAY_INVOKE_RESET_PROTOCOL);
    }
    query.version = 1U;
    query.max_cost_microunits = open->max_cost_microunits;
    query.max_latency_ms = open->max_latency_ms;
    if (!route_table_lookup_peer_target(
            manager->routes, &query, open->target_router_id,
            upstream_peer_id, now_ms, &selection)) {
        manager->stats.transit_no_route++;
        manager->stats.transit_failed++;
        manager->stats.failed++;
        return send_open_reset(manager, upstream_peer_id, open->stream_id,
                               RELAY_INVOKE_RESET_UNAVAILABLE);
    }
    slot = allocate_slot(manager);
    if (slot == NULL ||
        !copy_text(slot->peer_id, sizeof(slot->peer_id), upstream_peer_id) ||
        !copy_text(slot->downstream_peer_id,
                   sizeof(slot->downstream_peer_id),
                   selection.route->learned_from_peer)) {
        if (slot != NULL) clear_slot(slot);
        manager->stats.transit_failed++;
        manager->stats.failed++;
        return send_open_reset(manager, upstream_peer_id, open->stream_id,
                               RELAY_INVOKE_RESET_BACKPRESSURE);
    }
    slot->role = RELAY_INVOKE_TRANSIT_OPENING;
    slot->streaming = open->streaming;
    slot->stream_id = open->stream_id;
    slot->next_send_sequence = 1U;
    slot->open = *open;
    if (!set_timeout(slot, open->max_latency_ms)) {
        clear_slot(slot);
        manager->stats.transit_failed++;
        manager->stats.failed++;
        return send_open_reset(manager, upstream_peer_id, open->stream_id,
                               RELAY_INVOKE_RESET_UNAVAILABLE);
    }
    downstream_open = *open;
    downstream_open.hop_limit--;
    if (!agent_peer_transport_tunnel_open(
            manager->transport, slot->downstream_peer_id,
            &downstream_open) &&
        !agent_peer_listener_tunnel_open(
            manager->listener, slot->downstream_peer_id,
            &downstream_open)) {
        send_reset(slot, RELAY_INVOKE_RESET_UNAVAILABLE);
        manager->stats.transit_failed++;
        manager->stats.failed++;
        clear_slot(slot);
        return true;
    }
    slot->downstream_stream_id = downstream_open.stream_id;
    slot->downstream_next_send_sequence = 2U;
    manager->stats.transit_started++;
    return true;
}

static bool forward_transit_message(
    struct relay_invoke_slot *slot,
    enum relay_invoke_leg from_leg,
    const struct agent_relay_tunnel_message *message
)
{
    enum relay_invoke_leg to_leg = from_leg == RELAY_INVOKE_UPSTREAM
        ? RELAY_INVOKE_DOWNSTREAM : RELAY_INVOKE_UPSTREAM;
    struct agent_relay_tunnel_message forwarded = *message;

    if (!send_tunnel_leg(slot, to_leg, &forwarded)) return false;
    if (message->type == AGENT_RELAY_TUNNEL_DATA) {
        slot->manager->stats.transit_bytes_forwarded +=
            message->data_length;
    }
    refresh_timeout(slot);
    return true;
}

static void handle_transit_message(
    struct relay_invoke_slot *slot,
    enum relay_invoke_leg from_leg,
    const struct agent_relay_tunnel_message *message
)
{
    bool opening = slot->role == RELAY_INVOKE_TRANSIT_OPENING;

    if (message->type == AGENT_RELAY_TUNNEL_RESET) {
        (void)forward_transit_message(slot, from_leg, message);
        slot->manager->stats.transit_failed++;
        slot->manager->stats.failed++;
        clear_slot(slot);
        return;
    }
    if (opening) {
        if (from_leg != RELAY_INVOKE_DOWNSTREAM ||
            message->type != AGENT_RELAY_TUNNEL_ACCEPT ||
            !forward_transit_message(slot, from_leg, message)) {
            fail_slot(slot, 502U, "Transit target rejected invoke",
                      RELAY_INVOKE_RESET_UNAVAILABLE);
            return;
        }
        slot->role = RELAY_INVOKE_TRANSIT_FORWARDING;
        return;
    }
    if (message->type == AGENT_RELAY_TUNNEL_ACCEPT ||
        (from_leg == RELAY_INVOKE_UPSTREAM &&
         message->type == AGENT_RELAY_TUNNEL_RESPONSE_START) ||
        (message->type == AGENT_RELAY_TUNNEL_RESPONSE_START &&
         !slot->streaming) ||
        !forward_transit_message(slot, from_leg, message)) {
        fail_slot(slot, 502U, "invalid Transit Invoke frame",
                  RELAY_INVOKE_RESET_PROTOCOL);
        return;
    }
    if (message->type == AGENT_RELAY_TUNNEL_END) {
        if (from_leg == RELAY_INVOKE_UPSTREAM) slot->upstream_ended = true;
        else slot->downstream_ended = true;
        if (slot->upstream_ended && slot->downstream_ended) {
            slot->manager->stats.transit_completed++;
            slot->manager->stats.completed++;
            clear_slot(slot);
        }
    }
}

struct agent_relay_invoke_manager *agent_relay_invoke_create(
    struct route_table *routes,
    struct agent_peer_transport_manager *transport,
    const char *local_router_id,
    uint64_t (*now_ms)(void),
    const struct agent_relay_invoke_auth_config *auth,
    const struct agent_relay_invoke_gateway_config *gateway,
    const struct agent_relay_invoke_alias_config *alias
)
{
    struct agent_relay_invoke_manager *manager;
    size_t index;

    if (routes == NULL || transport == NULL || local_router_id == NULL ||
        now_ms == NULL) return NULL;
    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL ||
        !copy_text(manager->local_router_id,
                   sizeof(manager->local_router_id), local_router_id)) {
        goto fail;
    }
    manager->routes = routes;
    manager->transport = transport;
    manager->now_ms = now_ms;
    if (alias != NULL) {
        manager->tenant_alias_resolver = alias->resolve;
        manager->tenant_alias_context = alias->context;
    }
    if (gateway != NULL && gateway->enabled) {
        if (gateway->endpoint == NULL || gateway->token_file == NULL ||
            !agent_invoke_parse_loopback_endpoint(
                gateway->endpoint, &manager->internal_gateway_endpoint) ||
            strcmp(manager->internal_gateway_endpoint.path,
                   AGENT_INVOKE_INTERNAL_PATH) != 0 ||
            !load_internal_gateway_token(
                gateway->token_file, manager->internal_gateway_token)) {
            goto fail;
        }
        manager->internal_gateway_enabled = true;
    }
    if (auth != NULL && auth->required) {
        if (!copy_text(manager->forwarding_source_router_id,
                       sizeof(manager->forwarding_source_router_id),
                       auth->source_router_id)) {
            goto fail;
        }
        if (agent_forwarding_verifier_init(
                &manager->forwarding_verifier, auth->public_key_file,
                auth->key_id, auth->issuer, auth->clock_skew_seconds,
                auth->max_ttl_seconds) != AGENT_FORWARDING_OK ||
            !agent_replay_cache_init(
                &manager->forwarding_replay, auth->replay_capacity)) {
            agent_forwarding_verifier_free(
                &manager->forwarding_verifier);
            agent_replay_cache_free(&manager->forwarding_replay);
            goto fail;
        }
        manager->forwarding_required = true;
    }
    for (index = 0U; index < RELAY_INVOKE_MAX_TRANSACTIONS; index++) {
        manager->slots[index].manager = manager;
        manager->slots[index].backend_fd = -1;
    }
    return manager;

fail:
    if (manager != NULL) {
        erase_secret(manager->internal_gateway_token,
                     sizeof(manager->internal_gateway_token));
    }
    free(manager);
    return NULL;
}

void agent_relay_invoke_destroy(struct agent_relay_invoke_manager *manager)
{
    size_t index;

    if (manager == NULL) return;
    for (index = 0U; index < RELAY_INVOKE_MAX_TRANSACTIONS; index++) {
        struct relay_invoke_slot *slot = &manager->slots[index];
        if (slot->role == RELAY_INVOKE_SOURCE && slot->client != NULL) {
            (void)agent_ipc_client_respond_error(
                slot->client, slot->request_id, 503U,
                "agentd is shutting down");
            slot->client = NULL;
        }
        clear_slot(slot);
    }
    agent_replay_cache_free(&manager->forwarding_replay);
    agent_forwarding_verifier_free(&manager->forwarding_verifier);
    erase_secret(manager->internal_gateway_token,
                 sizeof(manager->internal_gateway_token));
    free(manager);
}

void agent_relay_invoke_set_listener(
    struct agent_relay_invoke_manager *manager,
    struct agent_peer_listener *listener
)
{
    if (manager != NULL) manager->listener = listener;
}

void agent_relay_invoke_set_open_mesh(
    struct agent_relay_invoke_manager *manager,
    struct peer_table *peers,
    bool enabled
)
{
    if (manager == NULL) return;
    manager->peers = peers;
    manager->open_mesh = enabled;
}

bool agent_relay_invoke_from_ipc(
    struct agent_relay_invoke_manager *manager,
    struct agent_ipc_client *client,
    const struct agent_ipc_invoke_request *request
)
{
    const struct agent_route *route;
    struct relay_invoke_slot *slot;
    struct agent_relay_tunnel_message message;

    if (manager == NULL || client == NULL || request == NULL) return false;
    route = route_table_find(manager->routes, request->route_id);
    if (route == NULL || route->source != AGENT_ROUTE_SOURCE_PEER ||
        route->path_length == 0U ||
        strcmp(route->learned_from_peer, request->relay_peer_id) != 0 ||
        strcmp(route->path[0], request->target_router_id) != 0) return false;
    slot = allocate_slot(manager);
    if (slot == NULL ||
        !copy_text(slot->peer_id, sizeof(slot->peer_id),
                   request->relay_peer_id)) {
        if (slot != NULL) clear_slot(slot);
        return false;
    }
    slot->role = RELAY_INVOKE_SOURCE;
    slot->streaming = request->streaming;
    slot->client = client;
    slot->request_id = request->request_id;
    slot->buffer = malloc(RELAY_INVOKE_MAX_WIRE_RESPONSE);
    slot->buffer_capacity = RELAY_INVOKE_MAX_WIRE_RESPONSE;
    if (slot->buffer == NULL || !set_timeout(slot, request->timeout_ms)) {
        clear_slot(slot);
        return false;
    }
    memcpy(slot->buffer, request->body, request->body_length);
    slot->buffer_length = request->body_length;
    memset(&message, 0, sizeof(message));
    message.type = AGENT_RELAY_TUNNEL_OPEN;
    message.streaming = request->streaming;
    message.hop_limit = request->hop_limit;
    message.max_cost_microunits = request->max_cost_microunits;
    message.max_latency_ms = request->max_latency_ms;
    if (!copy_text(message.target_router_id,
                   sizeof(message.target_router_id),
                   request->target_router_id) ||
        !copy_text(message.intent_class, sizeof(message.intent_class),
                   request->intent) ||
        !copy_text(message.task_id, sizeof(message.task_id),
                   request->task_id) ||
        !copy_text(message.source_agent, sizeof(message.source_agent),
                   request->source_agent) ||
        (request->target_agent[0] != '\0' &&
         !copy_text(message.target_agent, sizeof(message.target_agent),
                    request->target_agent)) ||
        !copy_text(message.tenant, sizeof(message.tenant), request->tenant) ||
        !copy_text(message.region, sizeof(message.region), request->region) ||
        (request->forwarding_assertion[0] != '\0' &&
         !copy_text(message.forwarding_assertion,
                    sizeof(message.forwarding_assertion),
                    request->forwarding_assertion)) ||
        (!agent_peer_transport_tunnel_open(
             manager->transport, slot->peer_id, &message) &&
         !agent_peer_listener_tunnel_open(
             manager->listener, slot->peer_id, &message))) {
        clear_slot(slot);
        return false;
    }
    slot->stream_id = message.stream_id;
    slot->next_send_sequence = 2U;
    manager->stats.source_started++;
    if (request->streaming) manager->stats.streams_started++;
    return true;
}

void agent_relay_invoke_cancel_ipc(
    struct agent_relay_invoke_manager *manager,
    struct agent_ipc_client *client
)
{
    size_t index;

    if (manager == NULL || client == NULL) return;
    for (index = 0U; index < RELAY_INVOKE_MAX_TRANSACTIONS; index++) {
        struct relay_invoke_slot *slot = &manager->slots[index];
        if (slot->role == RELAY_INVOKE_SOURCE && slot->client == client) {
            slot->client = NULL;
            send_reset(slot, RELAY_INVOKE_RESET_UNAVAILABLE);
            clear_slot(slot);
            return;
        }
    }
}

bool agent_relay_invoke_on_tunnel(
    struct agent_relay_invoke_manager *manager,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message,
    uint64_t now_ms
)
{
    struct relay_invoke_slot *slot;
    struct agent_relay_tunnel_message reply;
    enum relay_invoke_leg leg = RELAY_INVOKE_UPSTREAM;

    if (manager == NULL || peer_id == NULL || message == NULL) return false;
    if (message->type == AGENT_RELAY_TUNNEL_OPEN) {
        if (strcmp(message->target_router_id, manager->local_router_id) != 0) {
            return start_transit_invoke(manager, peer_id, message, now_ms);
        }
        {
            struct agent_forwarding_verified verified;
            const struct agent_peer *peer = manager->peers == NULL
                ? NULL : peer_table_find(manager->peers, peer_id);
            bool mesh_direct = manager->open_mesh && peer != NULL &&
                (peer->role != AGENT_PEER_ROLE_RELAY || peer->open_mesh);

            memset(&verified, 0, sizeof(verified));
            if (!verify_forwarding_open(
                    manager, message, &verified, mesh_direct)) {
                return send_open_reset(
                    manager, peer_id, message->stream_id,
                    RELAY_INVOKE_RESET_AUTHORIZATION);
            }
            slot = allocate_slot(manager);
            if (slot != NULL && manager->forwarding_required) {
                if (!mesh_direct) {
                    slot->forwarding_verified = true;
                    slot->forwarding = verified;
                }
            }
            if (slot != NULL) slot->mesh_direct = mesh_direct;
        }
        if (slot == NULL ||
            !copy_text(slot->peer_id, sizeof(slot->peer_id), peer_id)) {
            if (slot != NULL) clear_slot(slot);
            return false;
        }
        slot->role = RELAY_INVOKE_TARGET_RECEIVING;
        slot->streaming = message->streaming;
        slot->stream_id = message->stream_id;
        slot->next_send_sequence = 1U;
        slot->open = *message;
        slot->buffer = malloc(AGENT_IPC_MAX_INVOKE_BODY);
        slot->buffer_capacity = AGENT_IPC_MAX_INVOKE_BODY;
        if (slot->buffer == NULL ||
            !set_timeout(slot, message->max_latency_ms)) {
            clear_slot(slot);
            return false;
        }
        memset(&reply, 0, sizeof(reply));
        reply.type = AGENT_RELAY_TUNNEL_ACCEPT;
        reply.status_code = 200U;
        reply.credit_bytes = RELAY_INVOKE_INITIAL_CREDIT;
        if (!send_tunnel(slot, &reply)) {
            clear_slot(slot);
            return false;
        }
        manager->stats.target_started++;
        if (message->streaming) manager->stats.streams_started++;
        return true;
    }
    slot = find_stream(manager, peer_id, message->stream_id, &leg);
    if (slot == NULL) {
        /* The mux validates late control frames and bounded, sequenced DATA/
         * END already in flight when we locally reset a request. Do not
         * recreate its application state or tear down the shared session. */
        return message->type == AGENT_RELAY_TUNNEL_WINDOW_UPDATE ||
               message->type == AGENT_RELAY_TUNNEL_RESET ||
               message->type == AGENT_RELAY_TUNNEL_DATA ||
               message->type == AGENT_RELAY_TUNNEL_END;
    }
    if (slot->role == RELAY_INVOKE_TRANSIT_OPENING ||
        slot->role == RELAY_INVOKE_TRANSIT_FORWARDING) {
        handle_transit_message(slot, leg, message);
        return true;
    }
    if (message->type == AGENT_RELAY_TUNNEL_WINDOW_UPDATE &&
        slot->role == RELAY_INVOKE_TARGET_RESPONDING) {
        pump_target_response(slot);
        return true;
    }
    if (message->type == AGENT_RELAY_TUNNEL_ACCEPT) {
        if (slot->role != RELAY_INVOKE_SOURCE ||
            message->status_code != 200U) {
            fail_slot(slot, 502U, "Relay target rejected invoke",
                      RELAY_INVOKE_RESET_UNAVAILABLE);
        } else if (!send_window_update(slot, RELAY_INVOKE_INITIAL_CREDIT) ||
                   !send_bytes(slot)) {
            fail_slot(slot, 503U, "Relay request queue is full",
                      RELAY_INVOKE_RESET_BACKPRESSURE);
        } else {
            slot->buffer_length = 0U;
        }
        return true;
    }
    if (message->type == AGENT_RELAY_TUNNEL_RESPONSE_START) {
        if (slot->role != RELAY_INVOKE_SOURCE || !slot->streaming ||
            slot->response_started || message->status_code != 200U ||
            !agent_ipc_client_stream_start(
                slot->client, slot->request_id, 200U,
                "text/event-stream")) {
            fail_slot(slot, 502U, "invalid Relay SSE response start",
                      RELAY_INVOKE_RESET_PROTOCOL);
        } else {
            slot->response_started = true;
            refresh_timeout(slot);
        }
        return true;
    }
    if (message->type == AGENT_RELAY_TUNNEL_DATA) {
        if (slot->role == RELAY_INVOKE_SOURCE && slot->streaming) {
            if (!slot->response_started ||
                message->data_length >
                    RELAY_STREAM_MAX_BYTES - slot->stream_bytes ||
                !agent_ipc_client_stream_data(
                    slot->client, slot->request_id,
                    message->data, message->data_length) ||
                message->data_length > UINT32_MAX - slot->pending_credit) {
                manager->stats.stream_backpressure_resets++;
                fail_slot(slot, 503U, "Relay SSE IPC backpressure",
                          RELAY_INVOKE_RESET_BACKPRESSURE);
            } else {
                slot->pending_credit += (uint32_t)message->data_length;
                if (slot->pending_credit >= RELAY_INVOKE_CREDIT_BATCH) {
                    if (!send_window_update(slot, slot->pending_credit)) {
                        manager->stats.stream_backpressure_resets++;
                        fail_slot(slot, 503U, "Relay SSE credit queue is full",
                                  RELAY_INVOKE_RESET_BACKPRESSURE);
                        return true;
                    }
                    slot->pending_credit = 0U;
                }
                slot->stream_bytes += message->data_length;
                manager->stats.bytes_received += message->data_length;
                manager->stats.stream_bytes += message->data_length;
                refresh_timeout(slot);
            }
        } else if ((slot->role != RELAY_INVOKE_SOURCE &&
                    slot->role != RELAY_INVOKE_TARGET_RECEIVING) ||
                   !append_buffer(slot, message->data,
                                  message->data_length)) {
            fail_slot(slot, 502U, "Relay invoke exceeded its body bound",
                      RELAY_INVOKE_RESET_BACKPRESSURE);
        } else if (slot->role == RELAY_INVOKE_TARGET_RECEIVING &&
                   !send_window_update(slot,
                                       (uint32_t)message->data_length)) {
            fail_slot(slot, 503U, "Relay request flow-control failed",
                      RELAY_INVOKE_RESET_BACKPRESSURE);
        }
        return true;
    }
    if (message->type == AGENT_RELAY_TUNNEL_END) {
        if (slot->role == RELAY_INVOKE_SOURCE && slot->streaming) {
            if (!slot->response_started ||
                !agent_ipc_client_stream_end(
                    slot->client, slot->request_id, slot->stream_bytes)) {
                slot->client = NULL;
                manager->stats.failed++;
            } else {
                slot->client = NULL;
                manager->stats.completed++;
                manager->stats.streams_completed++;
            }
            clear_slot(slot);
        } else if (slot->role == RELAY_INVOKE_SOURCE) finish_source(slot);
        else if (slot->role == RELAY_INVOKE_TARGET_RECEIVING) {
            if (slot->manager->forwarding_required &&
                !slot->mesh_direct &&
                (!slot->forwarding_verified ||
                 agent_forwarding_verify_body(
                     &slot->forwarding, slot->buffer,
                     slot->buffer_length) != AGENT_FORWARDING_OK)) {
                slot->manager->stats.forwarding_assertion_body_mismatches++;
                slot->manager->stats.forwarding_assertions_rejected++;
                fail_slot(slot, 403U, "forwarding assertion body mismatch",
                          RELAY_INVOKE_RESET_AUTHORIZATION);
            } else if (slot->manager->forwarding_required &&
                       !slot->mesh_direct &&
                       !signed_envelope_target_matches(slot)) {
                slot->manager->stats.forwarding_assertions_rejected++;
                slot->manager->stats.cloud_tenant_alias_rejections++;
                fail_slot(slot, 403U,
                          "forwarding assertion target mismatch",
                          RELAY_INVOKE_RESET_AUTHORIZATION);
            } else if (!start_local_invoke(slot)) {
                fail_slot(slot, 502U, "no eligible local Agent endpoint",
                          RELAY_INVOKE_RESET_UNAVAILABLE);
            }
        } else {
            fail_slot(slot, 502U, "unexpected Relay END",
                      RELAY_INVOKE_RESET_PROTOCOL);
        }
        return true;
    }
    if (message->type == AGENT_RELAY_TUNNEL_RESET) {
        if (slot->role == RELAY_INVOKE_SOURCE && slot->client != NULL) {
            (void)agent_ipc_client_respond_error(
                slot->client, slot->request_id, 502U,
                "Relay stream was reset by target");
            slot->client = NULL;
        }
        manager->stats.failed++;
        clear_slot(slot);
        return true;
    }
    return true;
}

void agent_relay_invoke_get_stats(
    const struct agent_relay_invoke_manager *manager,
    struct agent_relay_invoke_stats *stats
)
{
    if (stats == NULL) return;
    if (manager == NULL) memset(stats, 0, sizeof(*stats));
    else *stats = manager->stats;
}

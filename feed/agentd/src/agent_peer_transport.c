#include "agent_peer_transport.h"

#include "agent_arpx_protocol.h"
#include "agent_peer_session.h"
#include "agent_peer_transport_contract.h"
#include "agent_relay_tunnel.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libubox/uloop.h>
#include <limits.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AGENT_RELAY_TICKET_REFRESH_PATH "/relay/ticket/refresh/v1"
#define AGENT_RELAY_TICKET_REFRESH_TYPE \
    "application/vnd.nexus.relay-ticket-refresh.v1"

enum transport_phase {
    TRANSPORT_DISABLED = 0,
    TRANSPORT_INELIGIBLE,
    TRANSPORT_INBOUND_ONLY,
    TRANSPORT_TCP_CONNECTING,
    TRANSPORT_TLS_HANDSHAKE,
    TRANSPORT_H2_STREAM,
    TRANSPORT_WAIT_OPEN,
    TRANSPORT_ESTABLISHED,
    TRANSPORT_BACKOFF
};

struct agent_peer_transport_manager;

struct peer_transport_slot {
    struct agent_peer_transport_manager *manager;
    char peer_id[AGENT_PEER_ID_LEN];
    char remote_router_id[AGENT_ROUTER_ID_LEN];
    char remote_domain_id[AGENT_DOMAIN_ID_LEN];
    char relay_session_ticket[AGENT_PEER_RELAY_TICKET_LEN];
    bool relay;
    bool failure_notified;
    struct agent_peer_transport_endpoint endpoint;
    bool eligible;
    enum transport_phase phase;
    struct uloop_fd watcher;
    mbedtls_ssl_context tls;
    bool tls_initialized;
    bool tls_want_write;
    bool tls_send_blocked_on_read;
    nghttp2_session *h2;
    int32_t stream_id;
    int32_t tunnel_stream_id;
    int32_t ticket_refresh_stream_id;
    bool ticket_refresh_status_ok;
    bool response_status_ok;
    bool response_content_type_ok;
    bool response_headers_complete;
    bool stream_closed;
    bool protocol_failure;
    bool tunnel_response_status_ok;
    bool tunnel_response_content_type_ok;
    bool tunnel_response_headers_complete;
    bool tunnel_stream_closed;
    struct agent_peer_session session;
    struct agent_peer_transport_ingress ingress;
    uint8_t tx_frame[AGENT_ARPX_MAX_FRAME_SIZE];
    size_t tx_length;
    size_t tx_offset;
    struct agent_arpx_message pending_messages[
        AGENT_PEER_TRANSPORT_QUEUE_CAPACITY];
    size_t pending_head;
    size_t pending_count;
    struct agent_relay_tunnel_ingress tunnel_ingress;
    struct agent_relay_mux tunnel_mux;
    uint8_t tunnel_tx_frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t tunnel_tx_length;
    size_t tunnel_tx_offset;
    uint8_t tunnel_pending_frames[AGENT_PEER_TUNNEL_QUEUE_CAPACITY]
        [AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t tunnel_pending_lengths[AGENT_PEER_TUNNEL_QUEUE_CAPACITY];
    size_t tunnel_pending_head;
    size_t tunnel_pending_count;
    uint64_t local_sequence;
    uint64_t next_heartbeat_ms;
    uint64_t tcp_connects;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t failures;
    char last_error[AGENT_PEER_TRANSPORT_ERROR_LEN];
};

struct agent_peer_transport_manager {
    struct agent_peer_transport_config config;
    struct peer_table *peers;
    uint64_t (*now_ms)(void);
    uint64_t boot_epoch;
    struct uloop_timeout timer;
    struct peer_transport_slot *slots;
    size_t slot_count;
    bool tls_initialized;
    mbedtls_x509_crt ca_chain;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config tls_config;
};

static const char *alpn_protocols[] = { "h2", NULL };

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || capacity == 0U || source == NULL) {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static void set_error(char *target, size_t capacity, const char *message)
{
    if (target != NULL && capacity > 0U) {
        (void)snprintf(target, capacity, "%s",
                       message == NULL ? "unknown error" : message);
    }
}

static const char *phase_name(enum transport_phase phase)
{
    switch (phase) {
    case TRANSPORT_DISABLED:
        return "disabled";
    case TRANSPORT_INELIGIBLE:
        return "ineligible";
    case TRANSPORT_INBOUND_ONLY:
        return "inbound-only";
    case TRANSPORT_TCP_CONNECTING:
        return "tcp-connecting";
    case TRANSPORT_TLS_HANDSHAKE:
        return "tls-handshake";
    case TRANSPORT_H2_STREAM:
        return "h2-stream";
    case TRANSPORT_WAIT_OPEN:
        return "wait-open";
    case TRANSPORT_ESTABLISHED:
        return "established";
    case TRANSPORT_BACKOFF:
        return "backoff";
    default:
        return "unknown";
    }
}

static struct agent_peer *mutable_peer(
    struct agent_peer_transport_manager *manager,
    const char *peer_id
)
{
    struct agent_peer *peer;

    if (manager == NULL || manager->peers == NULL) {
        return NULL;
    }
    for (peer = manager->peers->head; peer != NULL; peer = peer->next) {
        if (strcmp(peer->peer_id, peer_id) == 0) {
            return peer;
        }
    }
    return NULL;
}

static void update_peer_state(
    struct peer_transport_slot *slot,
    enum agent_peer_state state
)
{
    struct agent_peer *peer = mutable_peer(slot->manager, slot->peer_id);

    if (peer == NULL || peer->state == state) {
        return;
    }
    (void)peer_table_set_state(slot->manager->peers, slot->peer_id, state);
}

static void update_peer_session_fields(struct peer_transport_slot *slot)
{
    struct agent_peer *peer = mutable_peer(slot->manager, slot->peer_id);

    if (peer != NULL) {
        peer->boot_epoch = slot->session.remote_boot_epoch;
        peer->last_sequence = slot->session.last_sequence;
    }
}

static int tls_socket_send(
    void *context,
    const unsigned char *buffer,
    size_t length
)
{
    int fd = *(int *)context;
    ssize_t result;

    if (length > (size_t)INT_MAX) {
        length = (size_t)INT_MAX;
    }
    result = send(fd, buffer, length, MSG_NOSIGNAL);
    if (result >= 0) {
        return (int)result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_socket_receive(
    void *context,
    unsigned char *buffer,
    size_t length
)
{
    int fd = *(int *)context;
    ssize_t result;

    if (length > (size_t)INT_MAX) {
        length = (size_t)INT_MAX;
    }
    result = recv(fd, buffer, length, 0);
    if (result >= 0) {
        return (int)result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static void slot_cleanup_io(struct peer_transport_slot *slot)
{
    uint64_t tunnel_opened_local = slot->tunnel_mux.opened_local;
    uint64_t tunnel_opened_remote = slot->tunnel_mux.opened_remote;
    uint64_t tunnel_frames_sent = slot->tunnel_mux.frames_sent;
    uint64_t tunnel_frames_received = slot->tunnel_mux.frames_received;
    uint64_t tunnel_resets = slot->tunnel_mux.resets;
    uint64_t tunnel_protocol_errors = slot->tunnel_mux.protocol_errors;

    if (slot->h2 != NULL) {
        nghttp2_session_del(slot->h2);
        slot->h2 = NULL;
    }
    if (slot->watcher.registered) {
        uloop_fd_delete(&slot->watcher);
    }
    if (slot->watcher.fd >= 0) {
        close(slot->watcher.fd);
        slot->watcher.fd = -1;
    }
    if (slot->tls_initialized) {
        mbedtls_ssl_free(&slot->tls);
        slot->tls_initialized = false;
    }
    slot->stream_id = -1;
    slot->tunnel_stream_id = -1;
    slot->ticket_refresh_stream_id = -1;
    slot->ticket_refresh_status_ok = false;
    slot->tls_want_write = false;
    slot->tls_send_blocked_on_read = false;
    slot->response_status_ok = false;
    slot->response_content_type_ok = false;
    slot->response_headers_complete = false;
    slot->stream_closed = false;
    slot->protocol_failure = false;
    slot->tunnel_response_status_ok = false;
    slot->tunnel_response_content_type_ok = false;
    slot->tunnel_response_headers_complete = false;
    slot->tunnel_stream_closed = false;
    slot->tx_length = 0U;
    slot->tx_offset = 0U;
    slot->pending_head = 0U;
    slot->pending_count = 0U;
    slot->tunnel_tx_length = 0U;
    slot->tunnel_tx_offset = 0U;
    slot->tunnel_pending_head = 0U;
    slot->tunnel_pending_count = 0U;
    agent_peer_transport_ingress_init(&slot->ingress);
    agent_relay_tunnel_ingress_init(&slot->tunnel_ingress);
    if (slot->manager->config.relay_tunnel_enabled) {
        (void)agent_relay_mux_init(
            &slot->tunnel_mux, true, AGENT_RELAY_TUNNEL_MAX_STREAMS,
            AGENT_RELAY_TUNNEL_MIN_WINDOW);
        slot->tunnel_mux.opened_local = tunnel_opened_local;
        slot->tunnel_mux.opened_remote = tunnel_opened_remote;
        slot->tunnel_mux.frames_sent = tunnel_frames_sent;
        slot->tunnel_mux.frames_received = tunnel_frames_received;
        slot->tunnel_mux.resets = tunnel_resets;
        slot->tunnel_mux.protocol_errors = tunnel_protocol_errors;
    } else {
        memset(&slot->tunnel_mux, 0, sizeof(slot->tunnel_mux));
    }
}

static void slot_fail(struct peer_transport_slot *slot, const char *message)
{
    uint64_t now = slot->manager->now_ms();
    bool was_up = slot->phase == TRANSPORT_ESTABLISHED;

    slot->failures++;
    set_error(slot->last_error, sizeof(slot->last_error), message);
    slot_cleanup_io(slot);
    agent_peer_session_transport_down(&slot->session, now);
    slot->phase = TRANSPORT_BACKOFF;
    if ((was_up || (slot->relay && !slot->failure_notified)) &&
        slot->manager->config.session_handler != NULL) {
        if (slot->relay) slot->failure_notified = true;
        slot->manager->config.session_handler(
            slot->manager->config.event_context, slot->peer_id, false, now);
    } else {
        update_peer_state(slot, AGENT_PEER_STATE_DOWN);
    }
}

static void update_interest(struct peer_transport_slot *slot)
{
    unsigned int flags = ULOOP_READ;

    if (slot->watcher.fd < 0) {
        return;
    }
    if (slot->phase == TRANSPORT_TCP_CONNECTING ||
        (slot->phase == TRANSPORT_TLS_HANDSHAKE && slot->tls_want_write) ||
        (slot->h2 != NULL && nghttp2_session_want_write(slot->h2) != 0 &&
         !slot->tls_send_blocked_on_read)) {
        flags |= ULOOP_WRITE;
    }
    if (uloop_fd_add(&slot->watcher, flags) != 0) {
        slot_fail(slot, "failed to register peer socket with uloop");
    }
}

static bool activate_message(
    struct peer_transport_slot *slot,
    const struct agent_arpx_message *message
)
{
    size_t written = 0U;

    if (slot->tx_length != 0U || message == NULL) {
        return false;
    }
    if (agent_arpx_frame_encode(message, slot->tx_frame,
                                sizeof(slot->tx_frame), &written) !=
        AGENT_ARPX_OK) {
        return false;
    }
    slot->tx_length = written;
    slot->tx_offset = 0U;
    if (slot->h2 != NULL && slot->stream_id > 0) {
        int result = nghttp2_session_resume_data(slot->h2, slot->stream_id);

        if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT) {
            return false;
        }
    }
    return true;
}

static bool queue_payload(
    struct peer_transport_slot *slot,
    const struct agent_arpx_message *message
)
{
    size_t tail;

    if (slot->tx_length == 0U && slot->pending_count == 0U) {
        return activate_message(slot, message);
    }
    if (slot->pending_count >= AGENT_PEER_TRANSPORT_QUEUE_CAPACITY) {
        return false;
    }
    tail = (slot->pending_head + slot->pending_count) %
           AGENT_PEER_TRANSPORT_QUEUE_CAPACITY;
    slot->pending_messages[tail] = *message;
    slot->pending_count++;
    return true;
}

static bool activate_pending(struct peer_transport_slot *slot)
{
    struct agent_arpx_message message;

    if (slot->pending_count == 0U) {
        return true;
    }
    message = slot->pending_messages[slot->pending_head];
    slot->pending_head = (slot->pending_head + 1U) %
                         AGENT_PEER_TRANSPORT_QUEUE_CAPACITY;
    slot->pending_count--;
    return activate_message(slot, &message);
}

static bool activate_tunnel_frame(
    struct peer_transport_slot *slot,
    const uint8_t *frame,
    size_t frame_size
)
{
    if (slot->tunnel_tx_length != 0U || frame == NULL || frame_size == 0U ||
        frame_size > sizeof(slot->tunnel_tx_frame)) {
        return false;
    }
    memcpy(slot->tunnel_tx_frame, frame, frame_size);
    slot->tunnel_tx_length = frame_size;
    slot->tunnel_tx_offset = 0U;
    if (slot->h2 != NULL && slot->tunnel_stream_id > 0) {
        int result = nghttp2_session_resume_data(
            slot->h2, slot->tunnel_stream_id);

        if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT) {
            return false;
        }
    }
    return true;
}

static bool queue_tunnel_frame(
    struct peer_transport_slot *slot,
    const uint8_t *frame,
    size_t frame_size
)
{
    size_t tail;

    if (slot->tunnel_tx_length == 0U &&
        slot->tunnel_pending_count == 0U) {
        return activate_tunnel_frame(slot, frame, frame_size);
    }
    if (slot->tunnel_pending_count >= AGENT_PEER_TUNNEL_QUEUE_CAPACITY ||
        frame_size == 0U ||
        frame_size > AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE) {
        return false;
    }
    tail = (slot->tunnel_pending_head + slot->tunnel_pending_count) %
           AGENT_PEER_TUNNEL_QUEUE_CAPACITY;
    memcpy(slot->tunnel_pending_frames[tail], frame, frame_size);
    slot->tunnel_pending_lengths[tail] = frame_size;
    slot->tunnel_pending_count++;
    return true;
}

static bool activate_pending_tunnel(struct peer_transport_slot *slot)
{
    size_t frame_size;
    size_t head;

    if (slot->tunnel_pending_count == 0U) {
        return true;
    }
    head = slot->tunnel_pending_head;
    frame_size = slot->tunnel_pending_lengths[head];
    slot->tunnel_pending_head = (head + 1U) %
                                AGENT_PEER_TUNNEL_QUEUE_CAPACITY;
    slot->tunnel_pending_count--;
    return activate_tunnel_frame(
        slot, slot->tunnel_pending_frames[head], frame_size);
}

static bool queue_message(
    struct peer_transport_slot *slot,
    enum agent_arpx_message_type type,
    uint64_t sequence
)
{
    struct agent_arpx_message message;

    memset(&message, 0, sizeof(message));
    message.version = AGENT_ARPX_VERSION;
    message.type = type;
    if (!copy_text(message.router_id, sizeof(message.router_id),
                   slot->manager->config.local_router_id) ||
        !copy_text(message.domain_id, sizeof(message.domain_id),
                   slot->manager->config.local_domain_id)) {
        return false;
    }
    message.boot_epoch = slot->manager->boot_epoch;
    message.sequence = sequence;
    message.heartbeat_ms = type == AGENT_ARPX_OPEN
        ? slot->manager->config.heartbeat_ms : 0U;
    return queue_payload(slot, &message);
}

static nghttp2_ssize h2_send_callback(
    nghttp2_session *session,
    const uint8_t *data,
    size_t length,
    int flags,
    void *user_data
)
{
    struct peer_transport_slot *slot = user_data;
    int result;

    (void)session;
    (void)flags;
    if (length > (size_t)INT_MAX) {
        length = (size_t)INT_MAX;
    }
    result = mbedtls_ssl_write(&slot->tls, data, length);
    if (result > 0) {
        slot->tls_send_blocked_on_read = false;
        return (nghttp2_ssize)result;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        slot->tls_send_blocked_on_read = true;
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        slot->tls_send_blocked_on_read = false;
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static nghttp2_ssize h2_data_read_callback(
    nghttp2_session *session,
    int32_t stream_id,
    uint8_t *buffer,
    size_t length,
    uint32_t *data_flags,
    nghttp2_data_source *source,
    void *user_data
)
{
    struct peer_transport_slot *slot = source->ptr;
    size_t remaining;
    size_t copied;

    (void)session;
    (void)stream_id;
    (void)user_data;
    *data_flags = NGHTTP2_DATA_FLAG_NONE;
    if (slot->tx_length == 0U) {
        return NGHTTP2_ERR_DEFERRED;
    }
    remaining = slot->tx_length - slot->tx_offset;
    copied = remaining < length ? remaining : length;
    memcpy(buffer, slot->tx_frame + slot->tx_offset, copied);
    slot->tx_offset += copied;
    if (slot->tx_offset == slot->tx_length) {
        slot->tx_offset = 0U;
        slot->tx_length = 0U;
        slot->messages_sent++;
        if (!activate_pending(slot)) {
            slot->protocol_failure = true;
        }
    }
    return (nghttp2_ssize)copied;
}

static nghttp2_ssize h2_tunnel_data_read_callback(
    nghttp2_session *session,
    int32_t stream_id,
    uint8_t *buffer,
    size_t length,
    uint32_t *data_flags,
    nghttp2_data_source *source,
    void *user_data
)
{
    struct peer_transport_slot *slot = source->ptr;
    size_t remaining;
    size_t copied;

    (void)session;
    (void)stream_id;
    (void)user_data;
    *data_flags = NGHTTP2_DATA_FLAG_NONE;
    if (slot->tunnel_tx_length == 0U) {
        return NGHTTP2_ERR_DEFERRED;
    }
    remaining = slot->tunnel_tx_length - slot->tunnel_tx_offset;
    copied = remaining < length ? remaining : length;
    memcpy(buffer, slot->tunnel_tx_frame + slot->tunnel_tx_offset, copied);
    slot->tunnel_tx_offset += copied;
    if (slot->tunnel_tx_offset == slot->tunnel_tx_length) {
        slot->tunnel_tx_offset = 0U;
        slot->tunnel_tx_length = 0U;
        if (!activate_pending_tunnel(slot)) {
            slot->protocol_failure = true;
        }
    }
    return (nghttp2_ssize)copied;
}

static bool value_equals(
    const uint8_t *value,
    size_t length,
    const char *expected
)
{
    size_t expected_length = strlen(expected);

    return length == expected_length &&
           memcmp(value, expected, expected_length) == 0;
}

static int h2_header_callback(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    const uint8_t *name,
    size_t name_length,
    const uint8_t *value,
    size_t value_length,
    uint8_t flags,
    void *user_data
)
{
    struct peer_transport_slot *slot = user_data;

    (void)session;
    (void)flags;
    if ((frame->hd.stream_id != slot->stream_id &&
         frame->hd.stream_id != slot->tunnel_stream_id &&
         frame->hd.stream_id != slot->ticket_refresh_stream_id) ||
        frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
        return 0;
    }
    if (frame->hd.stream_id == slot->ticket_refresh_stream_id) {
        if (name_length == 7U && memcmp(name, ":status", 7U) == 0) {
            slot->ticket_refresh_status_ok = value_equals(
                value, value_length, "204");
        }
    } else if (frame->hd.stream_id == slot->stream_id) {
        if (name_length == 7U && memcmp(name, ":status", 7U) == 0) {
            slot->response_status_ok = value_equals(
                value, value_length, "200");
        } else if (name_length == 12U &&
                   memcmp(name, "content-type", 12U) == 0) {
            slot->response_content_type_ok = value_equals(
                value, value_length, AGENT_PEER_TRANSPORT_CONTENT_TYPE);
        }
    } else {
        if (name_length == 7U && memcmp(name, ":status", 7U) == 0) {
            slot->tunnel_response_status_ok = value_equals(
                value, value_length, "200");
        } else if (name_length == 12U &&
                   memcmp(name, "content-type", 12U) == 0) {
            slot->tunnel_response_content_type_ok = value_equals(
                value, value_length, AGENT_RELAY_TUNNEL_CONTENT_TYPE);
        }
    }
    return 0;
}

static int h2_frame_received_callback(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    void *user_data
)
{
    struct peer_transport_slot *slot = user_data;

    (void)session;
    if (frame->hd.stream_id == slot->ticket_refresh_stream_id) {
        if (frame->hd.type == NGHTTP2_HEADERS &&
            frame->headers.cat == NGHTTP2_HCAT_RESPONSE &&
            (!slot->ticket_refresh_status_ok ||
             (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0U)) {
            set_error(slot->last_error, sizeof(slot->last_error),
                      "Relay ticket refresh response rejected");
            slot->protocol_failure = true;
        }
        return 0;
    }
    if (frame->hd.stream_id != slot->stream_id &&
        frame->hd.stream_id != slot->tunnel_stream_id) {
        return 0;
    }
    if (frame->hd.stream_id == slot->tunnel_stream_id) {
        if (frame->hd.type == NGHTTP2_HEADERS &&
            frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            if (!slot->tunnel_response_status_ok ||
                !slot->tunnel_response_content_type_ok ||
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
                slot->protocol_failure = true;
                return 0;
            }
            slot->tunnel_response_headers_complete = true;
        } else if (frame->hd.type == NGHTTP2_DATA &&
                   (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
            slot->protocol_failure = true;
        }
        return 0;
    }
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        if (!slot->response_status_ok || !slot->response_content_type_ok ||
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
            set_error(slot->last_error, sizeof(slot->last_error),
                      "ARPX HTTP/2 response headers rejected");
            slot->protocol_failure = true;
            return 0;
        }
        if (!slot->response_headers_complete) {
            slot->response_headers_complete = true;
            if (agent_peer_session_transport_ready(
                    &slot->session, slot->manager->now_ms()) !=
                AGENT_PEER_SESSION_OK) {
                set_error(slot->last_error, sizeof(slot->last_error),
                          "ARPX transport-ready state rejected");
                slot->protocol_failure = true;
                return 0;
            }
            slot->phase = TRANSPORT_WAIT_OPEN;
        }
    } else if (frame->hd.type == NGHTTP2_DATA &&
               (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
        slot->protocol_failure = true;
    }
    return 0;
}

static bool receive_arpx_frame(
    const uint8_t *frame,
    size_t frame_size,
    void *context
)
{
    struct peer_transport_slot *slot = context;
    struct agent_arpx_message message;
    enum agent_peer_session_result result;

    if (agent_arpx_frame_decode(frame, frame_size, &message) != AGENT_ARPX_OK) {
        set_error(slot->last_error, sizeof(slot->last_error),
                  "ARPX response frame decode failed");
        return false;
    }
    {
    bool was_established =
        slot->session.state == AGENT_PEER_SESSION_ESTABLISHED;
    result = agent_peer_session_on_message(
        &slot->session, &message, slot->manager->now_ms());
    if (result != AGENT_PEER_SESSION_OK) {
        set_error(slot->last_error, sizeof(slot->last_error),
                  "ARPX OPEN identity or session rejected");
        return false;
    }
    slot->messages_received++;
    update_peer_session_fields(slot);
    if (slot->session.state == AGENT_PEER_SESSION_ESTABLISHED) {
        slot->phase = TRANSPORT_ESTABLISHED;
        slot->failure_notified = false;
        update_peer_state(slot, AGENT_PEER_STATE_ESTABLISHED);
        if (!was_established &&
            slot->manager->config.session_handler != NULL) {
            slot->manager->config.session_handler(
                slot->manager->config.event_context, slot->peer_id, true,
                slot->manager->now_ms());
        }
    }
    if ((message.type == AGENT_ARPX_CAPABILITY_UPDATE ||
         message.type == AGENT_ARPX_CAPABILITY_WITHDRAW ||
         message.type == AGENT_ARPX_SNAPSHOT_REQUEST ||
         message.type == AGENT_ARPX_SNAPSHOT_END) &&
        slot->manager->config.message_handler != NULL &&
        !slot->manager->config.message_handler(
            slot->manager->config.event_context, slot->peer_id, &message,
            slot->manager->now_ms())) {
        return false;
    }
    }
    return true;
}

static bool tunnel_queue_available(const struct peer_transport_slot *slot)
{
    return slot->tunnel_tx_length == 0U ||
           slot->tunnel_pending_count < AGENT_PEER_TUNNEL_QUEUE_CAPACITY;
}

static bool encode_and_queue_tunnel(
    struct peer_transport_slot *slot,
    const struct agent_relay_tunnel_message *message
)
{
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    return tunnel_queue_available(slot) &&
           agent_relay_tunnel_frame_encode(
               message, frame, sizeof(frame), &written) ==
               AGENT_RELAY_TUNNEL_OK &&
           queue_tunnel_frame(slot, frame, written);
}

static bool receive_tunnel_frame(
    const uint8_t *frame,
    size_t frame_size,
    void *context
)
{
    struct peer_transport_slot *slot = context;
    struct agent_relay_tunnel_message message;

    if (agent_relay_tunnel_frame_decode(frame, frame_size, &message) !=
            AGENT_RELAY_TUNNEL_OK ||
        agent_relay_mux_on_receive(&slot->tunnel_mux, &message) !=
            AGENT_RELAY_TUNNEL_OK) {
        return false;
    }
    if (message.type == AGENT_RELAY_TUNNEL_PING) {
        struct agent_relay_tunnel_message pong;

        memset(&pong, 0, sizeof(pong));
        pong.type = AGENT_RELAY_TUNNEL_PONG;
        pong.sequence = message.sequence;
        if (!tunnel_queue_available(slot) ||
            agent_relay_mux_on_send(&slot->tunnel_mux, &pong) !=
                AGENT_RELAY_TUNNEL_OK ||
            !encode_and_queue_tunnel(slot, &pong)) {
            return false;
        }
        return true;
    }
    return slot->manager->config.tunnel_handler == NULL ||
           slot->manager->config.tunnel_handler(
               slot->manager->config.event_context, slot->peer_id, &message,
               slot->manager->now_ms());
}

static int h2_data_received_callback(
    nghttp2_session *session,
    uint8_t flags,
    int32_t stream_id,
    const uint8_t *data,
    size_t length,
    void *user_data
)
{
    struct peer_transport_slot *slot = user_data;
    size_t emitted = 0U;
    enum agent_peer_transport_contract_result ingress_result;

    (void)session;
    (void)flags;
    if (stream_id == slot->ticket_refresh_stream_id) {
        if (length != 0U) {
            set_error(slot->last_error, sizeof(slot->last_error),
                      "Relay ticket refresh returned a response body");
            slot->protocol_failure = true;
        }
        return 0;
    }
    if (stream_id == slot->tunnel_stream_id) {
        if (slot->phase != TRANSPORT_ESTABLISHED ||
            !slot->tunnel_response_headers_complete ||
            agent_relay_tunnel_ingress_feed(
                &slot->tunnel_ingress, data, length, receive_tunnel_frame,
                slot, &emitted) != AGENT_RELAY_TUNNEL_OK) {
            slot->protocol_failure = true;
        }
        return 0;
    }
    if (stream_id != slot->stream_id) {
        set_error(slot->last_error, sizeof(slot->last_error),
                  "ARPX DATA arrived on an unknown HTTP/2 stream");
        slot->protocol_failure = true;
        return 0;
    }
    if (!slot->response_headers_complete) {
        set_error(slot->last_error, sizeof(slot->last_error),
                  "ARPX DATA arrived before accepted response headers");
        slot->protocol_failure = true;
        return 0;
    }
    ingress_result = agent_peer_transport_ingress_feed(
        &slot->ingress, data, length, receive_arpx_frame, slot, &emitted);
    if (ingress_result != AGENT_PEER_TRANSPORT_CONTRACT_OK) {
        if (slot->last_error[0] == '\0') {
            set_error(slot->last_error, sizeof(slot->last_error),
                      "ARPX framed ingress rejected response DATA");
        }
        slot->protocol_failure = true;
    }
    return 0;
}

static int h2_stream_closed_callback(
    nghttp2_session *session,
    int32_t stream_id,
    uint32_t error_code,
    void *user_data
)
{
    struct peer_transport_slot *slot = user_data;

    (void)session;
    (void)error_code;
    if (stream_id == slot->stream_id) {
        slot->stream_closed = true;
    } else if (stream_id == slot->tunnel_stream_id) {
        slot->tunnel_stream_closed = true;
    } else if (stream_id == slot->ticket_refresh_stream_id) {
        if (!slot->ticket_refresh_status_ok) {
            set_error(slot->last_error, sizeof(slot->last_error),
                      "Relay ticket refresh stream closed without acceptance");
            slot->protocol_failure = true;
        }
        slot->ticket_refresh_stream_id = -1;
        slot->ticket_refresh_status_ok = false;
    }
    return 0;
}

#define MAKE_NV(NAME, VALUE) \
    { (uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1U, \
      strlen(VALUE), NGHTTP2_NV_FLAG_NONE }
#define MAKE_SENSITIVE_NV(NAME, VALUE) \
    { (uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1U, \
      strlen(VALUE), NGHTTP2_NV_FLAG_NO_INDEX }

static bool start_h2(struct peer_transport_slot *slot)
{
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_data_provider2 provider;
    nghttp2_data_provider2 tunnel_provider;
    nghttp2_nv headers[] = {
        MAKE_NV(":method", "POST"),
        MAKE_NV(":scheme", "https"),
        MAKE_NV(":authority", slot->endpoint.authority),
        MAKE_NV(":path", AGENT_PEER_TRANSPORT_PATH),
        MAKE_NV("content-type", AGENT_PEER_TRANSPORT_CONTENT_TYPE),
        MAKE_NV("accept", AGENT_PEER_TRANSPORT_CONTENT_TYPE),
        MAKE_SENSITIVE_NV("nexus-relay-ticket", slot->relay_session_ticket)
    };
    nghttp2_nv tunnel_headers[] = {
        MAKE_NV(":method", "POST"),
        MAKE_NV(":scheme", "https"),
        MAKE_NV(":authority", slot->endpoint.authority),
        MAKE_NV(":path", AGENT_RELAY_TUNNEL_PATH),
        MAKE_NV("content-type", AGENT_RELAY_TUNNEL_CONTENT_TYPE),
        MAKE_NV("accept", AGENT_RELAY_TUNNEL_CONTENT_TYPE),
        MAKE_SENSITIVE_NV("nexus-relay-ticket", slot->relay_session_ticket)
    };
    size_t header_count = sizeof(headers) / sizeof(headers[0]);
    int result;

    result = nghttp2_session_callbacks_new(&callbacks);
    if (result != 0) {
        return false;
    }
    nghttp2_session_callbacks_set_send_callback2(callbacks, h2_send_callback);
    nghttp2_session_callbacks_set_on_header_callback(
        callbacks, h2_header_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
        callbacks, h2_frame_received_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, h2_data_received_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, h2_stream_closed_callback);
    result = nghttp2_session_client_new(&slot->h2, callbacks, slot);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0) {
        slot->h2 = NULL;
        return false;
    }

    /* RFC 9113 section 3.4 requires the client connection preface to be
     * followed by a SETTINGS frame.  nghttp2 does not enqueue one merely by
     * creating a client session, so it must precede the request HEADERS. */
    if (nghttp2_submit_settings(slot->h2, NGHTTP2_FLAG_NONE, NULL, 0U) != 0) {
        return false;
    }

    memset(&provider, 0, sizeof(provider));
    provider.source.ptr = slot;
    provider.read_callback = h2_data_read_callback;
    if (!queue_message(slot, AGENT_ARPX_OPEN, 1U)) {
        return false;
    }
    slot->local_sequence = 1U;
    slot->stream_id = nghttp2_submit_request2(
        slot->h2, NULL, headers, slot->relay ? header_count : header_count - 1U,
        &provider, slot);
    if (slot->stream_id < 0) {
        return false;
    }
    if (slot->manager->config.relay_tunnel_enabled) {
        memset(&tunnel_provider, 0, sizeof(tunnel_provider));
        tunnel_provider.source.ptr = slot;
        tunnel_provider.read_callback = h2_tunnel_data_read_callback;
        slot->tunnel_stream_id = nghttp2_submit_request2(
            slot->h2, NULL, tunnel_headers,
            sizeof(tunnel_headers) / sizeof(tunnel_headers[0]),
            &tunnel_provider, slot);
        if (slot->tunnel_stream_id < 0) {
            return false;
        }
    }
    slot->phase = TRANSPORT_H2_STREAM;
    slot->h2_sessions++;
    slot->next_heartbeat_ms = saturating_add(
        slot->manager->now_ms(), slot->manager->config.heartbeat_ms);
    return nghttp2_session_send(slot->h2) == 0;
}

static bool submit_ticket_refresh(struct peer_transport_slot *slot)
{
    nghttp2_nv headers[] = {
        MAKE_NV(":method", "POST"),
        MAKE_NV(":scheme", "https"),
        MAKE_NV(":authority", slot->endpoint.authority),
        MAKE_NV(":path", AGENT_RELAY_TICKET_REFRESH_PATH),
        MAKE_NV("content-type", AGENT_RELAY_TICKET_REFRESH_TYPE),
        MAKE_NV("accept", AGENT_RELAY_TICKET_REFRESH_TYPE),
        MAKE_SENSITIVE_NV("nexus-relay-ticket", slot->relay_session_ticket)
    };

    if (slot->h2 == NULL || slot->phase != TRANSPORT_ESTABLISHED ||
        slot->ticket_refresh_stream_id > 0) {
        return false;
    }
    slot->ticket_refresh_status_ok = false;
    slot->ticket_refresh_stream_id = nghttp2_submit_request2(
        slot->h2, NULL, headers, sizeof(headers) / sizeof(headers[0]),
        NULL, slot);
    if (slot->ticket_refresh_stream_id < 0) {
        slot->ticket_refresh_stream_id = -1;
        return false;
    }
    return nghttp2_session_send(slot->h2) == 0;
}

static bool start_tls(struct peer_transport_slot *slot)
{
    mbedtls_ssl_init(&slot->tls);
    if (mbedtls_ssl_setup(&slot->tls,
                          &slot->manager->tls_config) != 0 ||
        mbedtls_ssl_set_hostname(&slot->tls,
                                 slot->endpoint.server_identity) != 0) {
        mbedtls_ssl_free(&slot->tls);
        return false;
    }
    slot->tls_initialized = true;
    mbedtls_ssl_set_bio(&slot->tls, &slot->watcher.fd,
                        tls_socket_send, tls_socket_receive, NULL);
    slot->phase = TRANSPORT_TLS_HANDSHAKE;
    slot->tls_want_write = true;
    return true;
}

static void advance_tls(struct peer_transport_slot *slot)
{
    char detail[80];
    char message[AGENT_PEER_TRANSPORT_ERROR_LEN];
    const char *negotiated;
    int result = mbedtls_ssl_handshake(&slot->tls);

    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        slot->tls_want_write = false;
        update_interest(slot);
        return;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        slot->tls_want_write = true;
        update_interest(slot);
        return;
    }
    {
        uint32_t verify_flags = mbedtls_ssl_get_verify_result(&slot->tls);
        uint32_t open_mesh_allowed = MBEDTLS_X509_BADCERT_NOT_TRUSTED |
            MBEDTLS_X509_BADCERT_CN_MISMATCH;
        const struct agent_peer *configured_peer = peer_table_find(
            slot->manager->peers, slot->peer_id);
        bool open_mesh_peer = slot->manager->config.open_mesh &&
            (!slot->relay ||
             (configured_peer != NULL && configured_peer->open_mesh));
        const mbedtls_x509_crt *certificate =
            mbedtls_ssl_get_peer_cert(&slot->tls);

        if (result != 0 || certificate == NULL ||
            (!open_mesh_peer && verify_flags != 0U) ||
            (open_mesh_peer && (verify_flags & ~open_mesh_allowed) != 0U)) {

            mbedtls_strerror(result, detail, sizeof(detail));
            (void)snprintf(message, sizeof(message),
                           "TLS host=%.40s rc=-0x%04x verify=0x%08lx: %.48s",
                           slot->endpoint.server_identity,
                           result < 0 ? -result : result,
                           (unsigned long)verify_flags, detail);
            slot_fail(slot, message);
            return;
        }
    }
    negotiated = mbedtls_ssl_get_alpn_protocol(&slot->tls);
    if (negotiated == NULL || strcmp(negotiated, "h2") != 0) {
        slot_fail(slot, "TLS ALPN did not negotiate h2");
        return;
    }
    slot->tls_handshakes++;
    slot->tls_want_write = false;
    if (!start_h2(slot)) {
        slot_fail(slot, "failed to initialize HTTP/2 ARPX stream");
        return;
    }
    update_interest(slot);
}

static bool complete_tcp_connect(struct peer_transport_slot *slot)
{
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);

    if (getsockopt(slot->watcher.fd, SOL_SOCKET, SO_ERROR,
                   &socket_error, &length) != 0 || socket_error != 0) {
        return false;
    }
    slot->tcp_connects++;
    return start_tls(slot);
}

static bool feed_h2_bytes(
    struct peer_transport_slot *slot,
    const uint8_t *bytes,
    size_t length
)
{
    nghttp2_ssize consumed;

    consumed = nghttp2_session_mem_recv2(slot->h2, bytes, length);
    return consumed >= 0 && (size_t)consumed == length;
}

static bool read_h2(struct peer_transport_slot *slot)
{
    uint8_t buffer[8192];
    int result;

    slot->tls_send_blocked_on_read = false;
    for (;;) {
        result = mbedtls_ssl_read(&slot->tls, buffer, sizeof(buffer));
        if (result > 0) {
            if (!feed_h2_bytes(slot, buffer, (size_t)result)) {
                return false;
            }
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return true;
        }
        return false;
    }
}

static bool flush_h2(struct peer_transport_slot *slot)
{
    int result;

    slot->tls_send_blocked_on_read = false;
    result = nghttp2_session_send(slot->h2);
    return result == 0;
}

static void slot_io_callback(struct uloop_fd *watcher, unsigned int events)
{
    struct peer_transport_slot *slot =
        container_of(watcher, struct peer_transport_slot, watcher);

    if (watcher->error || watcher->eof) {
        slot_fail(slot, "peer socket closed");
        return;
    }
    if (slot->phase == TRANSPORT_TCP_CONNECTING) {
        if ((events & ULOOP_WRITE) == 0U) {
            return;
        }
        if (!complete_tcp_connect(slot)) {
            slot_fail(slot, "TCP connect failed");
            return;
        }
        advance_tls(slot);
        return;
    }
    if (slot->phase == TRANSPORT_TLS_HANDSHAKE) {
        advance_tls(slot);
        return;
    }
    if (slot->h2 == NULL) {
        slot_fail(slot, "HTTP/2 session is unavailable");
        return;
    }
    if ((events & ULOOP_READ) != 0U && !read_h2(slot)) {
        slot_fail(slot, "HTTP/2 TLS read failed");
        return;
    }
    if (slot->protocol_failure) {
        char detail[AGENT_PEER_TRANSPORT_ERROR_LEN];

        (void)copy_text(detail, sizeof(detail),
                        slot->last_error[0] != '\0'
                            ? slot->last_error
                            : "invalid HTTP/2 or ARPX peer message");
        slot_fail(slot, detail);
        return;
    }
    if (slot->stream_closed) {
        slot_fail(slot, "HTTP/2 ARPX stream closed");
        return;
    }
    if (slot->tunnel_stream_closed) {
        slot_fail(slot, "HTTP/2 Relay Invoke Tunnel stream closed");
        return;
    }
    if (((events & ULOOP_WRITE) != 0U ||
         nghttp2_session_want_write(slot->h2) != 0) &&
        !flush_h2(slot)) {
        slot_fail(slot, "HTTP/2 TLS write failed");
        return;
    }
    update_interest(slot);
}

static bool start_connect(struct peer_transport_slot *slot)
{
    struct sockaddr_in address;
    int fd;
    int result;

    slot_cleanup_io(slot);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(slot->endpoint.port);
    if (inet_pton(AF_INET, slot->endpoint.connect_ipv4,
                  &address.sin_addr) != 1) {
        close(fd);
        return false;
    }
    slot->watcher.fd = fd;
    slot->watcher.cb = slot_io_callback;
    slot->phase = TRANSPORT_TCP_CONNECTING;
    update_peer_state(slot, AGENT_PEER_STATE_CONNECTING);
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result == 0) {
        if (!start_tls(slot)) {
            return false;
        }
    } else if (errno != EINPROGRESS) {
        return false;
    }
    update_interest(slot);
    return true;
}

static void drive_heartbeat(struct peer_transport_slot *slot, uint64_t now)
{
    if (slot->phase != TRANSPORT_ESTABLISHED ||
        now < slot->next_heartbeat_ms || slot->tx_length != 0U ||
        slot->pending_count != 0U) {
        return;
    }
    if (slot->local_sequence == UINT64_MAX ||
        !queue_message(slot, AGENT_ARPX_HEARTBEAT,
                       slot->local_sequence + 1U)) {
        slot_fail(slot, "failed to queue bounded ARPX heartbeat");
        return;
    }
    slot->local_sequence++;
    slot->next_heartbeat_ms = saturating_add(
        now, slot->manager->config.heartbeat_ms);
    if (!flush_h2(slot)) {
        slot_fail(slot, "failed to flush ARPX heartbeat");
        return;
    }
    update_interest(slot);
}

static void manager_timer_callback(struct uloop_timeout *timeout)
{
    struct agent_peer_transport_manager *manager =
        container_of(timeout, struct agent_peer_transport_manager, timer);
    uint64_t now = manager->now_ms();
    size_t index;

    for (index = 0U; index < manager->slot_count; index++) {
        struct peer_transport_slot *slot = &manager->slots[index];

        if (!slot->eligible) {
            continue;
        }
        if (agent_peer_session_tick(&slot->session, now)) {
            if (slot->session.state == AGENT_PEER_SESSION_BACKOFF) {
                slot->failures++;
                set_error(slot->last_error, sizeof(slot->last_error),
                          "peer OPEN or heartbeat timeout");
                slot_cleanup_io(slot);
                slot->phase = TRANSPORT_BACKOFF;
                update_peer_state(slot, AGENT_PEER_STATE_DOWN);
            } else if (slot->session.state == AGENT_PEER_SESSION_CONNECTING) {
                slot->phase = TRANSPORT_TCP_CONNECTING;
                if (!start_connect(slot)) {
                    slot_fail(slot, "failed to start TCP reconnect");
                }
            }
        }
        drive_heartbeat(slot, now);
    }
    uloop_timeout_set(&manager->timer, (int)manager->config.tick_ms);
}

static bool random_boot_epoch(uint64_t *epoch)
{
    unsigned char bytes[sizeof(*epoch)];
    size_t offset = 0U;
    int fd;
    ssize_t received;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    while (offset < sizeof(bytes)) {
        received = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)received;
    }
    close(fd);
    memcpy(epoch, bytes, sizeof(*epoch));
    /* ubus JSON integers are signed; keep the diagnostic value positive. */
    *epoch &= UINT64_MAX >> 1U;
    if (*epoch == 0U) {
        *epoch = 1U;
    }
    return true;
}

static void free_tls_global(struct agent_peer_transport_manager *manager)
{
    if (!manager->tls_initialized) {
        return;
    }
    mbedtls_ssl_config_free(&manager->tls_config);
    mbedtls_pk_free(&manager->client_key);
    mbedtls_x509_crt_free(&manager->client_cert);
    mbedtls_x509_crt_free(&manager->ca_chain);
    mbedtls_ctr_drbg_free(&manager->drbg);
    mbedtls_entropy_free(&manager->entropy);
    manager->tls_initialized = false;
}

static bool initialize_tls_global(struct agent_peer_transport_manager *manager)
{
    static const unsigned char personalization[] =
        "nexus-agentd-arpx-p322";

    mbedtls_x509_crt_init(&manager->ca_chain);
    mbedtls_x509_crt_init(&manager->client_cert);
    mbedtls_pk_init(&manager->client_key);
    mbedtls_entropy_init(&manager->entropy);
    mbedtls_ctr_drbg_init(&manager->drbg);
    mbedtls_ssl_config_init(&manager->tls_config);
    manager->tls_initialized = true;
    if (mbedtls_x509_crt_parse_file(
            &manager->ca_chain, manager->config.ca_file) != 0 ||
        mbedtls_x509_crt_parse_file(
            &manager->client_cert,
            manager->config.client_cert_file) != 0 ||
        mbedtls_ctr_drbg_seed(
            &manager->drbg, mbedtls_entropy_func, &manager->entropy,
            personalization, sizeof(personalization) - 1U) != 0 ||
        mbedtls_pk_parse_keyfile(
            &manager->client_key, manager->config.client_key_file, NULL,
            mbedtls_ctr_drbg_random, &manager->drbg) != 0 ||
        mbedtls_ssl_config_defaults(
            &manager->tls_config, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    mbedtls_ssl_conf_authmode(
        &manager->tls_config,
        manager->config.open_mesh
            ? MBEDTLS_SSL_VERIFY_OPTIONAL
            : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&manager->tls_config,
                              &manager->ca_chain, NULL);
    mbedtls_ssl_conf_rng(&manager->tls_config,
                         mbedtls_ctr_drbg_random, &manager->drbg);
    mbedtls_ssl_conf_min_tls_version(&manager->tls_config,
                                     MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&manager->tls_config,
                                     MBEDTLS_SSL_VERSION_TLS1_3);
    if (mbedtls_ssl_conf_own_cert(
            &manager->tls_config, &manager->client_cert,
            &manager->client_key) != 0 ||
        mbedtls_ssl_conf_alpn_protocols(
            &manager->tls_config, alpn_protocols) != 0) {
        return false;
    }
    return true;
}

static bool config_valid(const struct agent_peer_transport_config *config)
{
    if (config == NULL || config->heartbeat_ms < AGENT_ARPX_MIN_HEARTBEAT_MS ||
        config->heartbeat_ms > AGENT_ARPX_MAX_HEARTBEAT_MS ||
        config->connect_timeout_ms < 100U ||
        config->connect_timeout_ms > 30000U ||
        config->open_timeout_ms < 100U || config->open_timeout_ms > 30000U ||
        config->heartbeat_miss_limit < 2U ||
        config->heartbeat_miss_limit > 10U ||
        config->initial_backoff_ms < 100U ||
        config->initial_backoff_ms > config->max_backoff_ms ||
        config->max_backoff_ms > 300000U ||
        config->tick_ms < 25U || config->tick_ms > 1000U) {
        return false;
    }
    if (config->relay_tunnel_enabled && !config->enabled) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    return agent_peer_id_valid(config->local_router_id,
                               sizeof(config->local_router_id)) &&
           agent_peer_domain_valid(config->local_domain_id) &&
           config->ca_file[0] != '\0' &&
           config->client_cert_file[0] != '\0' &&
           config->client_key_file[0] != '\0';
}

static void free_slots(struct agent_peer_transport_manager *manager)
{
    size_t index;

    uloop_timeout_cancel(&manager->timer);
    for (index = 0U; index < manager->slot_count; index++) {
        slot_cleanup_io(&manager->slots[index]);
    }
    free(manager->slots);
    manager->slots = NULL;
    manager->slot_count = 0U;
}

bool agent_peer_transport_reload(
    struct agent_peer_transport_manager *manager,
    struct peer_table *peers,
    char *error,
    size_t error_capacity
)
{
    const struct agent_peer *peer;
    struct peer_transport_slot *new_slots = NULL;
    size_t index = 0U;
    uint64_t now;

    if (manager == NULL || peers == NULL) {
        set_error(error, error_capacity, "invalid peer transport reload");
        return false;
    }
    if (peers->count > 0U) {
        new_slots = calloc(peers->count, sizeof(*new_slots));
    }
    if (peers->count > 0U && new_slots == NULL) {
        set_error(error, error_capacity, "failed to allocate peer transports");
        return false;
    }
    free_slots(manager);
    manager->peers = peers;
    manager->slots = new_slots;
    manager->slot_count = peers->count;
    if (peers->count == 0U) {
        return true;
    }
    now = manager->now_ms();
    for (peer = peer_table_first(peers); peer != NULL; peer = peer->next) {
        struct peer_transport_slot *slot = &manager->slots[index++];
        struct agent_peer_session_config session_config;

        memset(slot, 0, sizeof(*slot));
        slot->manager = manager;
        slot->watcher.fd = -1;
        slot->stream_id = -1;
        slot->tunnel_stream_id = -1;
        slot->ticket_refresh_stream_id = -1;
        slot->relay = peer->role == AGENT_PEER_ROLE_RELAY;
        (void)copy_text(slot->peer_id, sizeof(slot->peer_id), peer->peer_id);
        (void)copy_text(slot->remote_router_id,
                        sizeof(slot->remote_router_id), peer->router_id);
        (void)copy_text(slot->remote_domain_id,
                        sizeof(slot->remote_domain_id), peer->domain_id);
        (void)copy_text(slot->relay_session_ticket,
                        sizeof(slot->relay_session_ticket),
                        peer->relay_session_ticket);
        if (!manager->config.enabled) {
            slot->phase = TRANSPORT_DISABLED;
            continue;
        }
        /* Directory-assigned Relays are always dialed by the Node. */
        if (manager->config.deterministic_roles &&
            peer->role != AGENT_PEER_ROLE_RELAY) {
            enum agent_peer_transport_role role =
                agent_peer_transport_role_select(
                    manager->config.local_router_id, peer->router_id);

            if (role == AGENT_PEER_TRANSPORT_ROLE_INVALID) {
                slot->phase = TRANSPORT_INELIGIBLE;
                set_error(slot->last_error, sizeof(slot->last_error),
                          "local and remote router identity must differ");
                continue;
            }
            if (role == AGENT_PEER_TRANSPORT_ROLE_ACCEPT) {
                slot->phase = TRANSPORT_INBOUND_ONLY;
                continue;
            }
        }
        if (peer->connect_ipv4[0] == '\0' ||
            !agent_peer_transport_endpoint_parse(
                peer->endpoint, peer->connect_ipv4, &slot->endpoint)) {
            slot->phase = TRANSPORT_INELIGIBLE;
            set_error(slot->last_error, sizeof(slot->last_error),
                      "transport requires a valid pinned connect_ipv4");
            continue;
        }
        if (slot->relay && slot->relay_session_ticket[0] == '\0') {
            slot->phase = TRANSPORT_INELIGIBLE;
            set_error(slot->last_error, sizeof(slot->last_error),
                      "Relay assignment requires a session ticket");
            continue;
        }
        memset(&session_config, 0, sizeof(session_config));
        (void)copy_text(session_config.expected_router_id,
                        sizeof(session_config.expected_router_id),
                        peer->router_id);
        (void)copy_text(session_config.expected_domain_id,
                        sizeof(session_config.expected_domain_id),
                        peer->domain_id);
        session_config.open_timeout_ms = manager->config.open_timeout_ms;
        session_config.heartbeat_miss_limit =
            manager->config.heartbeat_miss_limit;
        session_config.initial_backoff_ms =
            manager->config.initial_backoff_ms;
        session_config.max_backoff_ms = manager->config.max_backoff_ms;
        if (!agent_peer_session_init(&slot->session, &session_config) ||
            agent_peer_session_begin(&slot->session, now) !=
                AGENT_PEER_SESSION_OK) {
            slot->phase = TRANSPORT_INELIGIBLE;
            set_error(slot->last_error, sizeof(slot->last_error),
                      "peer identity or session configuration is invalid");
            continue;
        }
        slot->session.deadline_ms = saturating_add(
            now, manager->config.connect_timeout_ms);
        slot->eligible = true;
        if (!start_connect(slot)) {
            slot_fail(slot, "failed to start bounded TCP connection");
        }
    }
    manager->timer.cb = manager_timer_callback;
    uloop_timeout_set(&manager->timer, (int)manager->config.tick_ms);
    return true;
}

struct agent_peer_transport_manager *agent_peer_transport_create(
    const struct agent_peer_transport_config *config,
    struct peer_table *peers,
    uint64_t (*now_ms)(void),
    char *error,
    size_t error_capacity
)
{
    struct agent_peer_transport_manager *manager;

    if (!config_valid(config) || peers == NULL || now_ms == NULL) {
        set_error(error, error_capacity, "invalid peer transport configuration");
        return NULL;
    }
    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) {
        set_error(error, error_capacity, "failed to allocate peer transport");
        return NULL;
    }
    manager->config = *config;
    manager->now_ms = now_ms;
    if (!random_boot_epoch(&manager->boot_epoch)) {
        set_error(error, error_capacity, "failed to create router boot epoch");
        free(manager);
        return NULL;
    }
    if (config->enabled && !initialize_tls_global(manager)) {
        set_error(error, error_capacity,
                  "failed to initialize mTLS peer identity");
        free_tls_global(manager);
        free(manager);
        return NULL;
    }
    if (!agent_peer_transport_reload(manager, peers, error, error_capacity)) {
        agent_peer_transport_destroy(manager);
        return NULL;
    }
    return manager;
}

void agent_peer_transport_destroy(
    struct agent_peer_transport_manager *manager
)
{
    if (manager == NULL) {
        return;
    }
    free_slots(manager);
    free_tls_global(manager);
    free(manager);
}

bool agent_peer_transport_get_status(
    const struct agent_peer_transport_manager *manager,
    const char *peer_id,
    struct agent_peer_transport_status *status
)
{
    size_t index;

    if (manager == NULL || peer_id == NULL || status == NULL) {
        return false;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        const struct peer_transport_slot *slot = &manager->slots[index];

        if (strcmp(slot->peer_id, peer_id) != 0) {
            continue;
        }
        memset(status, 0, sizeof(*status));
        status->configured = true;
        status->eligible = slot->eligible;
        status->session_up = slot->phase == TRANSPORT_ESTABLISHED;
        status->phase = phase_name(slot->phase);
        status->direction = slot->phase == TRANSPORT_INBOUND_ONLY
            ? "inbound" : "outbound";
        status->remote_boot_epoch = slot->session.remote_boot_epoch;
        status->remote_sequence = slot->session.last_sequence;
        status->local_sequence = slot->local_sequence;
        status->tcp_connects = slot->tcp_connects;
        status->tls_handshakes = slot->tls_handshakes;
        status->h2_sessions = slot->h2_sessions;
        status->messages_sent = slot->messages_sent;
        status->messages_received = slot->messages_received;
        status->protocol_errors = slot->session.protocol_errors;
        status->reconnects = slot->session.reconnects;
        status->relay_tunnel_enabled = manager->config.relay_tunnel_enabled;
        status->relay_tunnel_up = status->relay_tunnel_enabled &&
            slot->tunnel_response_headers_complete &&
            slot->phase == TRANSPORT_ESTABLISHED;
        status->relay_tunnel_streams = slot->tunnel_mux.active_streams;
        status->relay_tunnel_frames_sent = slot->tunnel_mux.frames_sent;
        status->relay_tunnel_frames_received =
            slot->tunnel_mux.frames_received;
        status->relay_tunnel_protocol_errors =
            slot->tunnel_mux.protocol_errors;
        (void)copy_text(status->last_error, sizeof(status->last_error),
                        slot->last_error);
        return true;
    }
    return false;
}

bool agent_peer_transport_set_relay_ticket(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    const char *session_ticket
)
{
    size_t index;
    size_t length;
    char previous_ticket[AGENT_PEER_RELAY_TICKET_LEN];

    if (manager == NULL || peer_id == NULL || session_ticket == NULL) {
        return false;
    }
    length = strnlen(session_ticket, AGENT_PEER_RELAY_TICKET_LEN);
    if (length == 0U || length >= AGENT_PEER_RELAY_TICKET_LEN) {
        return false;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        struct peer_transport_slot *slot = &manager->slots[index];

        if (strcmp(slot->peer_id, peer_id) != 0) continue;
        if (!slot->relay) return false;
        if (strcmp(slot->relay_session_ticket, session_ticket) == 0) {
            return true;
        }
        memcpy(previous_ticket, slot->relay_session_ticket,
               sizeof(previous_ticket));
        memcpy(slot->relay_session_ticket, session_ticket, length + 1U);
        if (slot->phase == TRANSPORT_ESTABLISHED &&
            !submit_ticket_refresh(slot)) {
            memcpy(slot->relay_session_ticket, previous_ticket,
                   sizeof(slot->relay_session_ticket));
            return false;
        }
        return true;
    }
    return false;
}

void agent_peer_transport_get_stats(
    const struct agent_peer_transport_manager *manager,
    struct agent_peer_transport_stats *stats
)
{
    size_t index;

    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (manager == NULL) {
        return;
    }
    stats->enabled = manager->config.enabled;
    stats->relay_tunnel_enabled = manager->config.relay_tunnel_enabled;
    stats->slots = manager->slot_count;
    for (index = 0U; index < manager->slot_count; index++) {
        const struct peer_transport_slot *slot = &manager->slots[index];

        if (slot->eligible) {
            stats->eligible++;
        }
        if (slot->phase == TRANSPORT_ESTABLISHED) {
            stats->sessions_up++;
        }
        stats->tcp_connects += slot->tcp_connects;
        stats->tls_handshakes += slot->tls_handshakes;
        stats->h2_sessions += slot->h2_sessions;
        stats->messages_sent += slot->messages_sent;
        stats->messages_received += slot->messages_received;
        stats->failures += slot->failures;
        if (slot->tunnel_response_headers_complete &&
            slot->phase == TRANSPORT_ESTABLISHED) {
            stats->relay_tunnels_up++;
        }
        stats->relay_tunnel_streams += slot->tunnel_mux.active_streams;
        stats->relay_tunnel_frames_sent += slot->tunnel_mux.frames_sent;
        stats->relay_tunnel_frames_received +=
            slot->tunnel_mux.frames_received;
        stats->relay_tunnel_protocol_errors +=
            slot->tunnel_mux.protocol_errors;
    }
}

uint64_t agent_peer_transport_boot_epoch(
    const struct agent_peer_transport_manager *manager
)
{
    return manager == NULL ? 0U : manager->boot_epoch;
}

bool agent_peer_transport_send(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    const struct agent_arpx_message *message
)
{
    struct agent_arpx_message outbound;
    size_t index;

    if (manager == NULL || peer_id == NULL || message == NULL ||
        (message->type != AGENT_ARPX_CAPABILITY_UPDATE &&
         message->type != AGENT_ARPX_CAPABILITY_WITHDRAW &&
         message->type != AGENT_ARPX_SNAPSHOT_REQUEST &&
         message->type != AGENT_ARPX_SNAPSHOT_END)) {
        return false;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        struct peer_transport_slot *slot = &manager->slots[index];

        if (strcmp(slot->peer_id, peer_id) != 0 ||
            slot->phase != TRANSPORT_ESTABLISHED ||
            slot->local_sequence == UINT64_MAX) {
            continue;
        }
        outbound = *message;
        outbound.version = AGENT_ARPX_VERSION;
        if (!copy_text(outbound.router_id, sizeof(outbound.router_id),
                       manager->config.local_router_id) ||
            !copy_text(outbound.domain_id, sizeof(outbound.domain_id),
                       manager->config.local_domain_id)) {
            return false;
        }
        outbound.boot_epoch = manager->boot_epoch;
        outbound.sequence = slot->local_sequence + 1U;
        outbound.heartbeat_ms = 0U;
        if (agent_arpx_message_validate(&outbound) != AGENT_ARPX_OK ||
            !queue_payload(slot, &outbound)) {
            return false;
        }
        slot->local_sequence++;
        update_interest(slot);
        return true;
    }
    return false;
}

bool agent_peer_transport_tunnel_open(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    struct agent_relay_tunnel_message *message
)
{
    size_t index;

    if (manager == NULL || peer_id == NULL || message == NULL ||
        message->type != AGENT_RELAY_TUNNEL_OPEN) {
        return false;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        struct peer_transport_slot *slot = &manager->slots[index];

        if (strcmp(slot->peer_id, peer_id) != 0 ||
            slot->phase != TRANSPORT_ESTABLISHED ||
            !slot->tunnel_response_headers_complete ||
            !tunnel_queue_available(slot)) {
            continue;
        }
        if (agent_relay_mux_open(&slot->tunnel_mux, message) !=
                AGENT_RELAY_TUNNEL_OK ||
            !encode_and_queue_tunnel(slot, message)) {
            return false;
        }
        update_interest(slot);
        return true;
    }
    return false;
}

bool agent_peer_transport_tunnel_send(
    struct agent_peer_transport_manager *manager,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message
)
{
    size_t index;

    if (manager == NULL || peer_id == NULL || message == NULL ||
        message->type == AGENT_RELAY_TUNNEL_OPEN) {
        return false;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        struct peer_transport_slot *slot = &manager->slots[index];

        if (strcmp(slot->peer_id, peer_id) != 0 ||
            slot->phase != TRANSPORT_ESTABLISHED ||
            !slot->tunnel_response_headers_complete ||
            !tunnel_queue_available(slot)) {
            continue;
        }
        if (agent_relay_tunnel_message_validate(message) !=
                AGENT_RELAY_TUNNEL_OK ||
            agent_relay_mux_on_send(&slot->tunnel_mux, message) !=
                AGENT_RELAY_TUNNEL_OK ||
            !encode_and_queue_tunnel(slot, message)) {
            return false;
        }
        update_interest(slot);
        return true;
    }
    return false;
}

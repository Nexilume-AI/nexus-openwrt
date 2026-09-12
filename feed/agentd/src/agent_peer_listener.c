#include "agent_peer_listener.h"

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

enum listener_peer_phase {
    LISTENER_PEER_DISABLED = 0,
    LISTENER_PEER_INELIGIBLE,
    LISTENER_PEER_WAIT,
    LISTENER_PEER_TLS,
    LISTENER_PEER_H2,
    LISTENER_PEER_WAIT_OPEN,
    LISTENER_PEER_ESTABLISHED
};

struct agent_peer_listener;
struct listener_connection;

struct listener_peer_slot {
    struct agent_peer_listener *manager;
    char peer_id[AGENT_PEER_ID_LEN];
    char remote_router_id[AGENT_ROUTER_ID_LEN];
    char remote_domain_id[AGENT_DOMAIN_ID_LEN];
    struct agent_peer_transport_endpoint endpoint;
    bool eligible;
    enum listener_peer_phase phase;
    struct listener_connection *connection;
    struct agent_peer_session session;
    uint64_t local_sequence;
    uint64_t next_heartbeat_ms;
    uint64_t accepts;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t failures;
    uint64_t protocol_errors;
    uint64_t reconnects;
    char last_error[AGENT_PEER_TRANSPORT_ERROR_LEN];
};

struct listener_connection {
    struct agent_peer_listener *manager;
    struct listener_connection *next;
    struct uloop_fd watcher;
    mbedtls_ssl_context tls;
    bool tls_initialized;
    bool tls_want_write;
    bool tls_send_blocked_on_read;
    nghttp2_session *h2;
    int32_t stream_id;
    int32_t tunnel_stream_id;
    bool method_ok;
    bool scheme_ok;
    bool authority_ok;
    bool path_ok;
    bool content_type_ok;
    bool accept_ok;
    bool headers_complete;
    bool open_validated;
    bool response_started;
    bool stream_closed;
    bool protocol_failure;
    bool tunnel_method_ok;
    bool tunnel_scheme_ok;
    bool tunnel_authority_ok;
    bool tunnel_path_ok;
    bool tunnel_content_type_ok;
    bool tunnel_accept_ok;
    bool tunnel_headers_complete;
    bool tunnel_response_started;
    bool tunnel_stream_closed;
    struct agent_peer_transport_ingress ingress;
    struct agent_relay_tunnel_ingress tunnel_ingress;
    struct agent_relay_mux tunnel_mux;
    struct listener_peer_slot *slot;
    uint8_t tx_frame[AGENT_ARPX_MAX_FRAME_SIZE];
    size_t tx_length;
    size_t tx_offset;
    struct agent_arpx_message pending_messages[
        AGENT_PEER_TRANSPORT_QUEUE_CAPACITY];
    size_t pending_head;
    size_t pending_count;
    uint8_t tunnel_tx_frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t tunnel_tx_length;
    size_t tunnel_tx_offset;
    uint8_t tunnel_pending_frames[AGENT_PEER_TUNNEL_QUEUE_CAPACITY]
        [AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t tunnel_pending_lengths[AGENT_PEER_TUNNEL_QUEUE_CAPACITY];
    size_t tunnel_pending_head;
    size_t tunnel_pending_count;
    uint64_t deadline_ms;
};

struct agent_peer_listener {
    struct agent_peer_listener_config config;
    struct peer_table *peers;
    uint64_t (*now_ms)(void);
    struct uloop_fd listener;
    struct uloop_timeout timer;
    struct listener_peer_slot *slots;
    size_t slot_count;
    struct listener_connection *connections;
    size_t active_connections;
    bool tls_initialized;
    mbedtls_x509_crt ca_chain;
    mbedtls_x509_crt server_cert;
    mbedtls_pk_context server_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config tls_config;
    uint64_t accepts;
    uint64_t tls_handshakes;
    uint64_t h2_sessions;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t failures;
    uint64_t rejected_connections;
};

static const char *listener_alpn_protocols[] = { "h2", NULL };

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

static const char *listener_phase_name(enum listener_peer_phase phase)
{
    switch (phase) {
    case LISTENER_PEER_DISABLED:
        return "disabled";
    case LISTENER_PEER_INELIGIBLE:
        return "outbound-only";
    case LISTENER_PEER_WAIT:
        return "listen-wait";
    case LISTENER_PEER_TLS:
        return "inbound-tls";
    case LISTENER_PEER_H2:
        return "inbound-h2";
    case LISTENER_PEER_WAIT_OPEN:
        return "inbound-wait-open";
    case LISTENER_PEER_ESTABLISHED:
        return "established";
    default:
        return "unknown";
    }
}

static struct agent_peer *mutable_peer(
    struct agent_peer_listener *manager,
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
    struct listener_peer_slot *slot,
    enum agent_peer_state state
)
{
    struct agent_peer *peer = mutable_peer(slot->manager, slot->peer_id);

    if (peer != NULL && peer->state != state) {
        (void)peer_table_set_state(slot->manager->peers, slot->peer_id, state);
    }
}

static void update_peer_session_fields(struct listener_peer_slot *slot)
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

static void unlink_connection(struct listener_connection *connection)
{
    struct listener_connection **link;
    struct agent_peer_listener *manager = connection->manager;

    for (link = &manager->connections; *link != NULL;
         link = &(*link)->next) {
        if (*link == connection) {
            *link = connection->next;
            if (manager->active_connections > 0U) {
                manager->active_connections--;
            }
            return;
        }
    }
}

static void close_connection(
    struct listener_connection *connection,
    const char *message,
    bool failure
)
{
    struct listener_peer_slot *slot;

    if (connection == NULL) {
        return;
    }
    slot = connection->slot;
    if (failure) {
        connection->manager->failures++;
    }
    if (slot != NULL) {
        bool was_up = slot->phase == LISTENER_PEER_ESTABLISHED;

        if (failure) {
            slot->failures++;
            slot->protocol_errors++;
            set_error(slot->last_error, sizeof(slot->last_error), message);
        }
        slot->connection = NULL;
        slot->phase = LISTENER_PEER_WAIT;
        memset(&slot->session, 0, sizeof(slot->session));
        if (was_up &&
            connection->manager->config.session.session_handler != NULL) {
            connection->manager->config.session.session_handler(
                connection->manager->config.session.event_context,
                slot->peer_id, false, connection->manager->now_ms());
        } else {
            update_peer_state(slot, AGENT_PEER_STATE_DOWN);
        }
    }
    if (connection->h2 != NULL) {
        nghttp2_session_del(connection->h2);
    }
    if (connection->watcher.registered) {
        uloop_fd_delete(&connection->watcher);
    }
    if (connection->watcher.fd >= 0) {
        close(connection->watcher.fd);
    }
    if (connection->tls_initialized) {
        mbedtls_ssl_free(&connection->tls);
    }
    unlink_connection(connection);
    free(connection);
}

static bool update_interest(struct listener_connection *connection)
{
    unsigned int flags = ULOOP_READ;

    if (connection->watcher.fd < 0) {
        return false;
    }
    if ((connection->h2 == NULL && connection->tls_want_write) ||
        (connection->h2 != NULL &&
         nghttp2_session_want_write(connection->h2) != 0 &&
         !connection->tls_send_blocked_on_read)) {
        flags |= ULOOP_WRITE;
    }
    return uloop_fd_add(&connection->watcher, flags) == 0;
}

static bool activate_message(
    struct listener_connection *connection,
    const struct agent_arpx_message *message
)
{
    size_t written = 0U;

    if (connection->tx_length != 0U || message == NULL) {
        return false;
    }
    if (agent_arpx_frame_encode(message, connection->tx_frame,
                                sizeof(connection->tx_frame), &written) !=
        AGENT_ARPX_OK) {
        return false;
    }
    connection->tx_length = written;
    connection->tx_offset = 0U;
    if (connection->response_started) {
        int result = nghttp2_session_resume_data(
            connection->h2, connection->stream_id);
        if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT) {
            return false;
        }
    }
    return true;
}

static bool queue_payload(
    struct listener_connection *connection,
    const struct agent_arpx_message *message
)
{
    size_t tail;

    if (connection->tx_length == 0U && connection->pending_count == 0U) {
        return activate_message(connection, message);
    }
    if (connection->pending_count >= AGENT_PEER_TRANSPORT_QUEUE_CAPACITY) {
        return false;
    }
    tail = (connection->pending_head + connection->pending_count) %
           AGENT_PEER_TRANSPORT_QUEUE_CAPACITY;
    connection->pending_messages[tail] = *message;
    connection->pending_count++;
    return true;
}

static bool activate_pending(struct listener_connection *connection)
{
    struct agent_arpx_message message;

    if (connection->pending_count == 0U) {
        return true;
    }
    message = connection->pending_messages[connection->pending_head];
    connection->pending_head = (connection->pending_head + 1U) %
                               AGENT_PEER_TRANSPORT_QUEUE_CAPACITY;
    connection->pending_count--;
    return activate_message(connection, &message);
}

static bool activate_tunnel_frame(
    struct listener_connection *connection,
    const uint8_t *frame,
    size_t frame_size
)
{
    if (connection->tunnel_tx_length != 0U || frame == NULL ||
        frame_size == 0U || frame_size > sizeof(connection->tunnel_tx_frame)) {
        return false;
    }
    memcpy(connection->tunnel_tx_frame, frame, frame_size);
    connection->tunnel_tx_length = frame_size;
    connection->tunnel_tx_offset = 0U;
    if (connection->tunnel_response_started) {
        int result = nghttp2_session_resume_data(
            connection->h2, connection->tunnel_stream_id);
        if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT) {
            return false;
        }
    }
    return true;
}

static bool queue_tunnel_frame(
    struct listener_connection *connection,
    const uint8_t *frame,
    size_t frame_size
)
{
    size_t tail;

    if (connection->tunnel_tx_length == 0U &&
        connection->tunnel_pending_count == 0U) {
        return activate_tunnel_frame(connection, frame, frame_size);
    }
    if (connection->tunnel_pending_count >=
            AGENT_PEER_TUNNEL_QUEUE_CAPACITY ||
        frame_size > AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE) {
        return false;
    }
    tail = (connection->tunnel_pending_head +
            connection->tunnel_pending_count) %
           AGENT_PEER_TUNNEL_QUEUE_CAPACITY;
    memcpy(connection->tunnel_pending_frames[tail], frame, frame_size);
    connection->tunnel_pending_lengths[tail] = frame_size;
    connection->tunnel_pending_count++;
    return true;
}

static bool activate_pending_tunnel(struct listener_connection *connection)
{
    size_t head;
    size_t frame_size;

    if (connection->tunnel_pending_count == 0U) return true;
    head = connection->tunnel_pending_head;
    frame_size = connection->tunnel_pending_lengths[head];
    connection->tunnel_pending_head = (head + 1U) %
                                      AGENT_PEER_TUNNEL_QUEUE_CAPACITY;
    connection->tunnel_pending_count--;
    return activate_tunnel_frame(
        connection, connection->tunnel_pending_frames[head], frame_size);
}

static bool queue_message(
    struct listener_connection *connection,
    enum agent_arpx_message_type type,
    uint64_t sequence
)
{
    struct agent_arpx_message message;

    memset(&message, 0, sizeof(message));
    message.version = AGENT_ARPX_VERSION;
    message.type = type;
    if (!copy_text(message.router_id, sizeof(message.router_id),
                   connection->manager->config.session.local_router_id) ||
        !copy_text(message.domain_id, sizeof(message.domain_id),
                   connection->manager->config.session.local_domain_id)) {
        return false;
    }
    message.boot_epoch = connection->manager->config.boot_epoch;
    message.sequence = sequence;
    message.heartbeat_ms = type == AGENT_ARPX_OPEN
        ? connection->manager->config.session.heartbeat_ms : 0U;
    return queue_payload(connection, &message);
}

static nghttp2_ssize h2_send_callback(
    nghttp2_session *session,
    const uint8_t *data,
    size_t length,
    int flags,
    void *user_data
)
{
    struct listener_connection *connection = user_data;
    int result;

    (void)session;
    (void)flags;
    if (length > (size_t)INT_MAX) {
        length = (size_t)INT_MAX;
    }
    result = mbedtls_ssl_write(&connection->tls, data, length);
    if (result > 0) {
        connection->tls_send_blocked_on_read = false;
        return (nghttp2_ssize)result;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        connection->tls_send_blocked_on_read = true;
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        connection->tls_send_blocked_on_read = false;
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
    struct listener_connection *connection = source->ptr;
    struct listener_peer_slot *slot = connection->slot;
    size_t remaining;
    size_t copied;

    (void)session;
    (void)stream_id;
    (void)user_data;
    *data_flags = NGHTTP2_DATA_FLAG_NONE;
    if (connection->tx_length == 0U) {
        return NGHTTP2_ERR_DEFERRED;
    }
    remaining = connection->tx_length - connection->tx_offset;
    copied = remaining < length ? remaining : length;
    memcpy(buffer, connection->tx_frame + connection->tx_offset, copied);
    connection->tx_offset += copied;
    if (connection->tx_offset == connection->tx_length) {
        connection->tx_offset = 0U;
        connection->tx_length = 0U;
        connection->manager->messages_sent++;
        if (slot != NULL) {
            slot->messages_sent++;
        }
        if (!activate_pending(connection)) {
            connection->protocol_failure = true;
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
    struct listener_connection *connection = source->ptr;
    size_t remaining;
    size_t copied;

    (void)session;
    (void)stream_id;
    (void)user_data;
    *data_flags = NGHTTP2_DATA_FLAG_NONE;
    if (connection->tunnel_tx_length == 0U) return NGHTTP2_ERR_DEFERRED;
    remaining = connection->tunnel_tx_length - connection->tunnel_tx_offset;
    copied = remaining < length ? remaining : length;
    memcpy(buffer, connection->tunnel_tx_frame +
           connection->tunnel_tx_offset, copied);
    connection->tunnel_tx_offset += copied;
    if (connection->tunnel_tx_offset == connection->tunnel_tx_length) {
        connection->tunnel_tx_offset = 0U;
        connection->tunnel_tx_length = 0U;
        if (!activate_pending_tunnel(connection)) {
            connection->protocol_failure = true;
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
    struct listener_connection *connection = user_data;
    bool tunnel;

    (void)session;
    (void)flags;
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    if (connection->stream_id == -1) {
        connection->stream_id = frame->hd.stream_id;
    } else if (frame->hd.stream_id != connection->stream_id &&
               connection->tunnel_stream_id == -1) {
        connection->tunnel_stream_id = frame->hd.stream_id;
    }
    if (frame->hd.stream_id != connection->stream_id &&
        frame->hd.stream_id != connection->tunnel_stream_id) {
        connection->protocol_failure = true;
        return 0;
    }
    tunnel = frame->hd.stream_id == connection->tunnel_stream_id;
    if ((!tunnel && connection->headers_complete) ||
        (tunnel && connection->tunnel_headers_complete)) {
        connection->protocol_failure = true;
        return 0;
    }
    if (name_length == 7U && memcmp(name, ":method", 7U) == 0) {
        if (tunnel) connection->tunnel_method_ok =
            value_equals(value, value_length, "POST");
        else connection->method_ok = value_equals(value, value_length, "POST");
    } else if (name_length == 7U && memcmp(name, ":scheme", 7U) == 0) {
        if (tunnel) connection->tunnel_scheme_ok =
            value_equals(value, value_length, "https");
        else connection->scheme_ok = value_equals(value, value_length, "https");
    } else if (name_length == 10U &&
               memcmp(name, ":authority", 10U) == 0) {
        if (tunnel) connection->tunnel_authority_ok =
            value_length > 0U && value_length < 192U;
        else connection->authority_ok =
            value_length > 0U && value_length < 192U;
    } else if (name_length == 5U && memcmp(name, ":path", 5U) == 0) {
        if (tunnel) connection->tunnel_path_ok = value_equals(
            value, value_length, AGENT_RELAY_TUNNEL_PATH);
        else connection->path_ok = value_equals(
            value, value_length, AGENT_PEER_TRANSPORT_PATH);
    } else if (name_length == 12U &&
               memcmp(name, "content-type", 12U) == 0) {
        if (tunnel) connection->tunnel_content_type_ok = value_equals(
            value, value_length, AGENT_RELAY_TUNNEL_CONTENT_TYPE);
        else connection->content_type_ok = value_equals(
            value, value_length, AGENT_PEER_TRANSPORT_CONTENT_TYPE);
    } else if (name_length == 6U && memcmp(name, "accept", 6U) == 0) {
        if (tunnel) connection->tunnel_accept_ok = value_equals(
            value, value_length, AGENT_RELAY_TUNNEL_CONTENT_TYPE);
        else connection->accept_ok = value_equals(
            value, value_length, AGENT_PEER_TRANSPORT_CONTENT_TYPE);
    }
    return 0;
}

static struct listener_peer_slot *find_slot_by_identity(
    struct agent_peer_listener *manager,
    const char *router_id,
    const char *domain_id
)
{
    size_t index;

    for (index = 0U; index < manager->slot_count; index++) {
        struct listener_peer_slot *slot = &manager->slots[index];

        if (slot->eligible &&
            strcmp(slot->remote_router_id, router_id) == 0 &&
            strcmp(slot->remote_domain_id, domain_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static bool peer_verify_flags_allowed(
    const struct agent_peer_listener *manager,
    uint32_t verify_flags
)
{
    uint32_t allowed;

    if (!manager->config.session.open_mesh) {
        return verify_flags == 0U;
    }
    allowed = MBEDTLS_X509_BADCERT_NOT_TRUSTED |
        MBEDTLS_X509_BADCERT_CN_MISMATCH;
    return (verify_flags & ~allowed) == 0U;
}

static bool verify_peer_certificate(
    struct listener_connection *connection,
    const struct listener_peer_slot *slot
)
{
    const mbedtls_x509_crt *certificate =
        mbedtls_ssl_get_peer_cert(&connection->tls);
    uint32_t flags = 0U;

    if (certificate == NULL) {
        return false;
    }
    if (connection->manager->config.session.open_mesh) {
        uint32_t verify_flags = mbedtls_ssl_get_verify_result(
            &connection->tls);
        return peer_verify_flags_allowed(connection->manager, verify_flags);
    }
    return mbedtls_x509_crt_verify(
        (mbedtls_x509_crt *)certificate,
        &connection->manager->ca_chain, NULL,
        slot->endpoint.server_identity, &flags, NULL, NULL) == 0 &&
        flags == 0U;
}

static bool initialize_inbound_session(
    struct listener_peer_slot *slot,
    uint64_t now
)
{
    struct agent_peer_session_config session_config;

    memset(&session_config, 0, sizeof(session_config));
    if (!copy_text(session_config.expected_router_id,
                   sizeof(session_config.expected_router_id),
                   slot->remote_router_id) ||
        !copy_text(session_config.expected_domain_id,
                   sizeof(session_config.expected_domain_id),
                   slot->remote_domain_id)) {
        return false;
    }
    session_config.open_timeout_ms =
        slot->manager->config.session.open_timeout_ms;
    session_config.heartbeat_miss_limit =
        slot->manager->config.session.heartbeat_miss_limit;
    session_config.initial_backoff_ms =
        slot->manager->config.session.initial_backoff_ms;
    session_config.max_backoff_ms =
        slot->manager->config.session.max_backoff_ms;
    return agent_peer_session_init(&slot->session, &session_config) &&
           agent_peer_session_begin(&slot->session, now) ==
               AGENT_PEER_SESSION_OK &&
           agent_peer_session_transport_ready(&slot->session, now) ==
               AGENT_PEER_SESSION_OK;
}

static bool receive_arpx_frame(
    const uint8_t *frame,
    size_t frame_size,
    void *context
)
{
    struct listener_connection *connection = context;
    struct agent_peer_listener *manager = connection->manager;
    struct agent_arpx_message message;
    struct listener_peer_slot *slot = connection->slot;
    uint64_t now = manager->now_ms();

    if (agent_arpx_frame_decode(frame, frame_size, &message) != AGENT_ARPX_OK) {
        return false;
    }
    if (slot == NULL) {
        slot = find_slot_by_identity(
            manager, message.router_id, message.domain_id);
        if (message.type != AGENT_ARPX_OPEN || slot == NULL ||
            slot->connection != NULL ||
            !verify_peer_certificate(connection, slot) ||
            !initialize_inbound_session(slot, now)) {
            return false;
        }
        update_peer_state(slot, AGENT_PEER_STATE_CONNECTING);
        if (agent_peer_session_on_message(&slot->session, &message, now) !=
            AGENT_PEER_SESSION_OK) {
            return false;
        }
        connection->slot = slot;
        slot->connection = connection;
        if (slot->accepts > 0U) {
            slot->reconnects++;
        }
        slot->accepts++;
        slot->tls_handshakes++;
        slot->h2_sessions++;
        slot->messages_received++;
        slot->local_sequence = 1U;
        slot->next_heartbeat_ms = saturating_add(
            now, manager->config.session.heartbeat_ms);
        slot->phase = LISTENER_PEER_ESTABLISHED;
        update_peer_session_fields(slot);
        update_peer_state(slot, AGENT_PEER_STATE_ESTABLISHED);
        manager->messages_received++;
        connection->open_validated = true;
        if (!queue_message(connection, AGENT_ARPX_OPEN, 1U)) {
            return false;
        }
        if (manager->config.session.session_handler != NULL) {
            manager->config.session.session_handler(
                manager->config.session.event_context, slot->peer_id, true,
                now);
        }
        return true;
    }
    if (agent_peer_session_on_message(&slot->session, &message, now) !=
        AGENT_PEER_SESSION_OK) {
        return false;
    }
    slot->messages_received++;
    manager->messages_received++;
    update_peer_session_fields(slot);
    if ((message.type == AGENT_ARPX_CAPABILITY_UPDATE ||
         message.type == AGENT_ARPX_CAPABILITY_WITHDRAW ||
         message.type == AGENT_ARPX_SNAPSHOT_REQUEST ||
         message.type == AGENT_ARPX_SNAPSHOT_END) &&
        manager->config.session.message_handler != NULL &&
        !manager->config.session.message_handler(
            manager->config.session.event_context, slot->peer_id, &message,
            now)) {
        return false;
    }
    return true;
}

static bool tunnel_queue_available(const struct listener_connection *connection)
{
    return connection->tunnel_tx_length == 0U ||
           connection->tunnel_pending_count <
               AGENT_PEER_TUNNEL_QUEUE_CAPACITY;
}

static bool encode_and_queue_tunnel(
    struct listener_connection *connection,
    const struct agent_relay_tunnel_message *message
)
{
    uint8_t frame[AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE];
    size_t written = 0U;

    return tunnel_queue_available(connection) &&
           agent_relay_tunnel_frame_encode(
               message, frame, sizeof(frame), &written) ==
               AGENT_RELAY_TUNNEL_OK &&
           queue_tunnel_frame(connection, frame, written);
}

static bool receive_tunnel_frame(
    const uint8_t *frame,
    size_t frame_size,
    void *context
)
{
    struct listener_connection *connection = context;
    struct agent_relay_tunnel_message message;

    if (connection->slot == NULL ||
        agent_relay_tunnel_frame_decode(frame, frame_size, &message) !=
            AGENT_RELAY_TUNNEL_OK ||
        agent_relay_mux_on_receive(&connection->tunnel_mux, &message) !=
            AGENT_RELAY_TUNNEL_OK) {
        return false;
    }
    if (message.type == AGENT_RELAY_TUNNEL_PING) {
        struct agent_relay_tunnel_message pong;

        memset(&pong, 0, sizeof(pong));
        pong.type = AGENT_RELAY_TUNNEL_PONG;
        pong.stream_id = message.stream_id;
        pong.sequence = message.sequence;
        return agent_relay_mux_on_send(&connection->tunnel_mux, &pong) ==
                   AGENT_RELAY_TUNNEL_OK &&
               encode_and_queue_tunnel(connection, &pong);
    }
    return connection->manager->config.session.tunnel_handler == NULL ||
           connection->manager->config.session.tunnel_handler(
               connection->manager->config.session.event_context,
               connection->slot->peer_id, &message,
               connection->manager->now_ms());
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
    struct listener_connection *connection = user_data;
    size_t emitted = 0U;

    (void)session;
    (void)flags;
    if (stream_id == connection->tunnel_stream_id) {
        if (!connection->tunnel_headers_complete ||
            agent_relay_tunnel_ingress_feed(
                &connection->tunnel_ingress, data, length,
                receive_tunnel_frame, connection, &emitted) !=
                AGENT_RELAY_TUNNEL_OK) {
            connection->protocol_failure = true;
        }
    } else if (stream_id != connection->stream_id ||
               !connection->headers_complete ||
               agent_peer_transport_ingress_feed(
            &connection->ingress, data, length, receive_arpx_frame,
            connection, &emitted) != AGENT_PEER_TRANSPORT_CONTRACT_OK) {
        connection->protocol_failure = true;
    }
    return 0;
}

static int h2_frame_received_callback(
    nghttp2_session *session,
    const nghttp2_frame *frame,
    void *user_data
)
{
    struct listener_connection *connection = user_data;

    (void)session;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        if (frame->hd.stream_id == connection->tunnel_stream_id) {
            if (connection->tunnel_headers_complete ||
                !connection->manager->config.session.relay_tunnel_enabled ||
                !connection->tunnel_method_ok ||
                !connection->tunnel_scheme_ok ||
                !connection->tunnel_authority_ok ||
                !connection->tunnel_path_ok ||
                !connection->tunnel_content_type_ok ||
                !connection->tunnel_accept_ok ||
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
                connection->protocol_failure = true;
            } else {
                connection->tunnel_headers_complete = true;
            }
        } else {
            if (connection->headers_complete ||
                frame->hd.stream_id != connection->stream_id ||
                !connection->method_ok || !connection->scheme_ok ||
                !connection->authority_ok || !connection->path_ok ||
                !connection->content_type_ok || !connection->accept_ok ||
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
                connection->protocol_failure = true;
            } else {
                connection->headers_complete = true;
                if (connection->slot != NULL) {
                    connection->slot->phase = LISTENER_PEER_WAIT_OPEN;
                }
            }
        }
    } else if (frame->hd.type == NGHTTP2_DATA &&
               (frame->hd.stream_id == connection->stream_id ||
                frame->hd.stream_id == connection->tunnel_stream_id) &&
               (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U) {
        connection->protocol_failure = true;
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
    struct listener_connection *connection = user_data;

    (void)session;
    (void)error_code;
    if (stream_id == connection->stream_id) {
        connection->stream_closed = true;
    } else if (stream_id == connection->tunnel_stream_id) {
        connection->tunnel_stream_closed = true;
    }
    return 0;
}

#define MAKE_NV(NAME, VALUE) \
    { (uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1U, \
      strlen(VALUE), NGHTTP2_NV_FLAG_NONE }

static bool submit_response(struct listener_connection *connection)
{
    nghttp2_data_provider2 provider;
    nghttp2_nv headers[] = {
        MAKE_NV(":status", "200"),
        MAKE_NV("content-type", AGENT_PEER_TRANSPORT_CONTENT_TYPE)
    };

    if (connection->response_started || !connection->headers_complete ||
        !connection->open_validated || connection->stream_id <= 0) {
        return connection->response_started;
    }
    memset(&provider, 0, sizeof(provider));
    provider.source.ptr = connection;
    provider.read_callback = h2_data_read_callback;
    if (nghttp2_submit_response2(
            connection->h2, connection->stream_id, headers,
            sizeof(headers) / sizeof(headers[0]), &provider) != 0) {
        return false;
    }
    connection->response_started = true;
    return true;
}

static bool submit_tunnel_response(struct listener_connection *connection)
{
    nghttp2_data_provider2 provider;
    nghttp2_nv headers[] = {
        MAKE_NV(":status", "200"),
        MAKE_NV("content-type", AGENT_RELAY_TUNNEL_CONTENT_TYPE)
    };

    if (connection->tunnel_response_started ||
        !connection->tunnel_headers_complete ||
        connection->tunnel_stream_id <= 0) {
        return connection->tunnel_response_started;
    }
    memset(&provider, 0, sizeof(provider));
    provider.source.ptr = connection;
    provider.read_callback = h2_tunnel_data_read_callback;
    if (nghttp2_submit_response2(
            connection->h2, connection->tunnel_stream_id, headers,
            sizeof(headers) / sizeof(headers[0]), &provider) != 0) {
        return false;
    }
    connection->tunnel_response_started = true;
    return true;
}

static bool start_h2(struct listener_connection *connection)
{
    nghttp2_session_callbacks *callbacks = NULL;
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
    result = nghttp2_session_server_new(
        &connection->h2, callbacks, connection);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0) {
        connection->h2 = NULL;
        return false;
    }
    if (nghttp2_submit_settings(
            connection->h2, NGHTTP2_FLAG_NONE, NULL, 0U) != 0) {
        return false;
    }
    connection->manager->h2_sessions++;
    return nghttp2_session_send(connection->h2) == 0;
}

static bool feed_h2_bytes(
    struct listener_connection *connection,
    const uint8_t *bytes,
    size_t length
)
{
    nghttp2_ssize consumed = nghttp2_session_mem_recv2(
        connection->h2, bytes, length);

    if (consumed < 0 || (size_t)consumed != length) {
        return false;
    }
    return (!connection->open_validated || connection->response_started ||
            submit_response(connection)) &&
           (!connection->tunnel_headers_complete ||
            connection->tunnel_response_started ||
            submit_tunnel_response(connection));
}

static bool read_h2(struct listener_connection *connection)
{
    uint8_t buffer[8192];
    int result;

    connection->tls_send_blocked_on_read = false;
    for (;;) {
        result = mbedtls_ssl_read(&connection->tls, buffer, sizeof(buffer));
        if (result > 0) {
            if (!feed_h2_bytes(connection, buffer, (size_t)result)) {
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

static bool flush_h2(struct listener_connection *connection)
{
    connection->tls_send_blocked_on_read = false;
    return nghttp2_session_send(connection->h2) == 0;
}

static void advance_tls(struct listener_connection *connection)
{
    char detail[80];
    char message[AGENT_PEER_TRANSPORT_ERROR_LEN];
    const char *negotiated;
    int result = mbedtls_ssl_handshake(&connection->tls);

    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        connection->tls_want_write = false;
        if (!update_interest(connection)) {
            close_connection(connection, "failed to watch inbound TLS", true);
        }
        return;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        connection->tls_want_write = true;
        if (!update_interest(connection)) {
            close_connection(connection, "failed to watch inbound TLS", true);
        }
        return;
    }
    {
        uint32_t verify_flags = mbedtls_ssl_get_verify_result(&connection->tls);
        const mbedtls_x509_crt *certificate =
            mbedtls_ssl_get_peer_cert(&connection->tls);

        if (result != 0 || certificate == NULL ||
            !peer_verify_flags_allowed(connection->manager, verify_flags)) {
            mbedtls_strerror(result, detail, sizeof(detail));
            (void)snprintf(message, sizeof(message),
                           "inbound TLS rc=-0x%04x verify=0x%08lx: %.64s",
                           result < 0 ? -result : result,
                           (unsigned long)verify_flags, detail);
            close_connection(connection, message, true);
            return;
        }
    }
    negotiated = mbedtls_ssl_get_alpn_protocol(&connection->tls);
    if (negotiated == NULL || strcmp(negotiated, "h2") != 0) {
        close_connection(connection, "inbound TLS ALPN is not h2", true);
        return;
    }
    connection->manager->tls_handshakes++;
    connection->tls_want_write = false;
    if (!start_h2(connection)) {
        close_connection(connection,
                         "failed to initialize inbound HTTP/2", true);
        return;
    }
    if (!update_interest(connection)) {
        close_connection(connection, "failed to watch inbound HTTP/2", true);
    }
}

static void connection_io_callback(
    struct uloop_fd *watcher,
    unsigned int events
)
{
    struct listener_connection *connection =
        container_of(watcher, struct listener_connection, watcher);

    if (watcher->error || watcher->eof) {
        close_connection(connection, "inbound peer socket closed", true);
        return;
    }
    if (connection->h2 == NULL) {
        advance_tls(connection);
        return;
    }
    if ((events & ULOOP_READ) != 0U && !read_h2(connection)) {
        close_connection(connection, "inbound HTTP/2 TLS read failed", true);
        return;
    }
    if (connection->protocol_failure) {
        close_connection(connection,
                         "invalid inbound HTTP/2 or ARPX message", true);
        return;
    }
    if (connection->stream_closed) {
        close_connection(connection, "inbound ARPX stream closed", true);
        return;
    }
    if (connection->tunnel_stream_closed) {
        close_connection(connection, "inbound invoke tunnel closed", true);
        return;
    }
    if (((events & ULOOP_WRITE) != 0U ||
         nghttp2_session_want_write(connection->h2) != 0) &&
        !flush_h2(connection)) {
        close_connection(connection, "inbound HTTP/2 TLS write failed", true);
        return;
    }
    if (!update_interest(connection)) {
        close_connection(connection, "failed to update inbound I/O", true);
    }
}

static bool start_inbound_tls(struct listener_connection *connection)
{
    mbedtls_ssl_init(&connection->tls);
    if (mbedtls_ssl_setup(
            &connection->tls, &connection->manager->tls_config) != 0) {
        mbedtls_ssl_free(&connection->tls);
        return false;
    }
    connection->tls_initialized = true;
    connection->tls_want_write = false;
    mbedtls_ssl_set_bio(&connection->tls, &connection->watcher.fd,
                        tls_socket_send, tls_socket_receive, NULL);
    return true;
}

static void listener_readable(
    struct uloop_fd *watcher,
    unsigned int events
)
{
    struct agent_peer_listener *manager =
        container_of(watcher, struct agent_peer_listener, listener);

    if ((events & ULOOP_READ) == 0U) {
        return;
    }
    for (;;) {
        struct listener_connection *connection;
        int fd = accept4(manager->listener.fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (manager->active_connections >= manager->config.max_connections) {
            manager->rejected_connections++;
            close(fd);
            continue;
        }
        connection = calloc(1U, sizeof(*connection));
        if (connection == NULL) {
            manager->rejected_connections++;
            close(fd);
            continue;
        }
        connection->manager = manager;
        connection->watcher.fd = fd;
        connection->watcher.cb = connection_io_callback;
        connection->stream_id = -1;
        connection->tunnel_stream_id = -1;
        connection->deadline_ms = saturating_add(
            manager->now_ms(), manager->config.session.open_timeout_ms);
        agent_peer_transport_ingress_init(&connection->ingress);
        agent_relay_tunnel_ingress_init(&connection->tunnel_ingress);
        if (manager->config.session.relay_tunnel_enabled &&
            !agent_relay_mux_init(
                &connection->tunnel_mux, false,
                AGENT_RELAY_TUNNEL_MAX_STREAMS,
                AGENT_RELAY_TUNNEL_MIN_WINDOW)) {
            free(connection);
            manager->rejected_connections++;
            close(fd);
            continue;
        }
        connection->next = manager->connections;
        manager->connections = connection;
        manager->active_connections++;
        manager->accepts++;
        if (!start_inbound_tls(connection) || !update_interest(connection)) {
            close_connection(connection,
                             "failed to initialize inbound TLS", true);
        }
    }
}

static bool send_heartbeat(struct listener_peer_slot *slot, uint64_t now)
{
    struct listener_connection *connection = slot->connection;

    if (slot->phase != LISTENER_PEER_ESTABLISHED || connection == NULL ||
        now < slot->next_heartbeat_ms || connection->tx_length != 0U ||
        connection->pending_count != 0U) {
        return true;
    }
    if (slot->local_sequence == UINT64_MAX ||
        !queue_message(connection, AGENT_ARPX_HEARTBEAT,
                       slot->local_sequence + 1U)) {
        return false;
    }
    slot->local_sequence++;
    slot->next_heartbeat_ms = saturating_add(
        now, slot->manager->config.session.heartbeat_ms);
    return flush_h2(connection) && update_interest(connection);
}

static void timer_callback(struct uloop_timeout *timeout)
{
    struct agent_peer_listener *manager =
        container_of(timeout, struct agent_peer_listener, timer);
    struct listener_connection *connection = manager->connections;
    uint64_t now = manager->now_ms();
    size_t index;

    while (connection != NULL) {
        struct listener_connection *next = connection->next;
        struct listener_peer_slot *slot = connection->slot;

        if (slot == NULL && now >= connection->deadline_ms) {
            close_connection(connection, "inbound OPEN timeout", true);
        } else if (slot != NULL &&
                   agent_peer_session_tick(&slot->session, now)) {
            close_connection(connection, "inbound heartbeat timeout", true);
        }
        connection = next;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        struct listener_peer_slot *slot = &manager->slots[index];

        if (!send_heartbeat(slot, now) && slot->connection != NULL) {
            close_connection(slot->connection,
                             "failed to send inbound heartbeat", true);
        }
    }
    uloop_timeout_set(&manager->timer,
                      (int)manager->config.session.tick_ms);
}

static bool session_config_valid(
    const struct agent_peer_transport_config *config
)
{
    return config != NULL &&
           agent_peer_id_valid(config->local_router_id,
                               sizeof(config->local_router_id)) &&
           agent_peer_domain_valid(config->local_domain_id) &&
           config->ca_file[0] != '\0' &&
           config->client_cert_file[0] != '\0' &&
           config->client_key_file[0] != '\0' &&
           config->heartbeat_ms >= AGENT_ARPX_MIN_HEARTBEAT_MS &&
           config->heartbeat_ms <= AGENT_ARPX_MAX_HEARTBEAT_MS &&
           config->open_timeout_ms >= 100U &&
           config->open_timeout_ms <= 30000U &&
           config->heartbeat_miss_limit >= 2U &&
           config->heartbeat_miss_limit <= 10U &&
           config->initial_backoff_ms >= 100U &&
           config->initial_backoff_ms <= config->max_backoff_ms &&
           config->max_backoff_ms <= 300000U &&
           config->tick_ms >= 25U && config->tick_ms <= 1000U;
}

static bool config_valid(const struct agent_peer_listener_config *config)
{
    if (config == NULL) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    return agent_peer_transport_ipv4_valid(config->listen_ipv4) &&
           config->listen_port != 0U &&
           config->max_connections >= 1U &&
           config->max_connections <= 64U &&
           config->boot_epoch != 0U && session_config_valid(&config->session);
}

static void free_tls_global(struct agent_peer_listener *manager)
{
    if (!manager->tls_initialized) {
        return;
    }
    mbedtls_ssl_config_free(&manager->tls_config);
    mbedtls_pk_free(&manager->server_key);
    mbedtls_x509_crt_free(&manager->server_cert);
    mbedtls_x509_crt_free(&manager->ca_chain);
    mbedtls_ctr_drbg_free(&manager->drbg);
    mbedtls_entropy_free(&manager->entropy);
    manager->tls_initialized = false;
}

static bool initialize_tls_global(struct agent_peer_listener *manager)
{
    static const unsigned char personalization[] =
        "nexus-agentd-arpx-p323";

    mbedtls_x509_crt_init(&manager->ca_chain);
    mbedtls_x509_crt_init(&manager->server_cert);
    mbedtls_pk_init(&manager->server_key);
    mbedtls_entropy_init(&manager->entropy);
    mbedtls_ctr_drbg_init(&manager->drbg);
    mbedtls_ssl_config_init(&manager->tls_config);
    manager->tls_initialized = true;
    if (mbedtls_x509_crt_parse_file(
            &manager->ca_chain, manager->config.session.ca_file) != 0 ||
        mbedtls_x509_crt_parse_file(
            &manager->server_cert,
            manager->config.session.client_cert_file) != 0 ||
        mbedtls_ctr_drbg_seed(
            &manager->drbg, mbedtls_entropy_func, &manager->entropy,
            personalization, sizeof(personalization) - 1U) != 0 ||
        mbedtls_pk_parse_keyfile(
            &manager->server_key,
            manager->config.session.client_key_file, NULL,
            mbedtls_ctr_drbg_random, &manager->drbg) != 0 ||
        mbedtls_ssl_config_defaults(
            &manager->tls_config, MBEDTLS_SSL_IS_SERVER,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    mbedtls_ssl_conf_authmode(
        &manager->tls_config,
        manager->config.session.open_mesh
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
    return mbedtls_ssl_conf_own_cert(
               &manager->tls_config, &manager->server_cert,
               &manager->server_key) == 0 &&
           mbedtls_ssl_conf_alpn_protocols(
               &manager->tls_config, listener_alpn_protocols) == 0;
}

static bool start_listener_socket(struct agent_peer_listener *manager)
{
    struct sockaddr_in address;
    int reuse = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return false;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(manager->config.listen_port);
    if (inet_pton(AF_INET, manager->config.listen_ipv4,
                  &address.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, (int)manager->config.max_connections) != 0) {
        close(fd);
        return false;
    }
    manager->listener.fd = fd;
    manager->listener.cb = listener_readable;
    if (uloop_fd_add(&manager->listener, ULOOP_READ) != 0) {
        close(fd);
        manager->listener.fd = -1;
        return false;
    }
    return true;
}

static void close_all_connections(struct agent_peer_listener *manager)
{
    while (manager->connections != NULL) {
        close_connection(manager->connections, NULL, false);
    }
}

static void free_slots(struct agent_peer_listener *manager)
{
    close_all_connections(manager);
    free(manager->slots);
    manager->slots = NULL;
    manager->slot_count = 0U;
}

bool agent_peer_listener_reload(
    struct agent_peer_listener *manager,
    struct peer_table *peers,
    char *error,
    size_t error_capacity
)
{
    const struct agent_peer *peer;
    struct listener_peer_slot *slots = NULL;
    size_t index = 0U;

    if (manager == NULL || peers == NULL) {
        set_error(error, error_capacity, "invalid peer listener reload");
        return false;
    }
    if (peers->count > 0U) {
        slots = calloc(peers->count, sizeof(*slots));
        if (slots == NULL) {
            set_error(error, error_capacity,
                      "failed to allocate inbound peer slots");
            return false;
        }
    }
    free_slots(manager);
    manager->peers = peers;
    manager->slots = slots;
    manager->slot_count = peers->count;
    for (peer = peer_table_first(peers); peer != NULL; peer = peer->next) {
        struct listener_peer_slot *slot = &manager->slots[index++];

        slot->manager = manager;
        (void)copy_text(slot->peer_id, sizeof(slot->peer_id), peer->peer_id);
        (void)copy_text(slot->remote_router_id,
                        sizeof(slot->remote_router_id), peer->router_id);
        (void)copy_text(slot->remote_domain_id,
                        sizeof(slot->remote_domain_id), peer->domain_id);
        if (!manager->config.enabled) {
            slot->phase = LISTENER_PEER_DISABLED;
            continue;
        }
        if (peer->role == AGENT_PEER_ROLE_RELAY) {
            slot->phase = LISTENER_PEER_INELIGIBLE;
            continue;
        }
        switch (agent_peer_transport_role_select(
                    manager->config.session.local_router_id,
                    peer->router_id)) {
        case AGENT_PEER_TRANSPORT_ROLE_DIAL:
            slot->phase = LISTENER_PEER_INELIGIBLE;
            continue;
        case AGENT_PEER_TRANSPORT_ROLE_INVALID:
            slot->phase = LISTENER_PEER_INELIGIBLE;
            set_error(slot->last_error, sizeof(slot->last_error),
                      "local and remote router identity must differ");
            continue;
        case AGENT_PEER_TRANSPORT_ROLE_ACCEPT:
            break;
        }
        if (peer->connect_ipv4[0] == '\0' ||
            !agent_peer_transport_endpoint_parse(
                peer->endpoint, peer->connect_ipv4, &slot->endpoint)) {
            slot->phase = LISTENER_PEER_INELIGIBLE;
            set_error(slot->last_error, sizeof(slot->last_error),
                      "inbound identity requires a valid peer endpoint pin");
            continue;
        }
        slot->eligible = true;
        slot->phase = LISTENER_PEER_WAIT;
    }
    return true;
}

struct agent_peer_listener *agent_peer_listener_create(
    const struct agent_peer_listener_config *config,
    struct peer_table *peers,
    uint64_t (*now_ms)(void),
    char *error,
    size_t error_capacity
)
{
    struct agent_peer_listener *manager;

    if (!config_valid(config) || peers == NULL || now_ms == NULL) {
        set_error(error, error_capacity,
                  "invalid inbound peer listener configuration");
        return NULL;
    }
    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) {
        set_error(error, error_capacity, "failed to allocate peer listener");
        return NULL;
    }
    manager->config = *config;
    manager->now_ms = now_ms;
    manager->listener.fd = -1;
    if (config->enabled &&
        (!initialize_tls_global(manager) || !start_listener_socket(manager))) {
        set_error(error, error_capacity,
                  "failed to initialize inbound mTLS HTTP/2 listener");
        agent_peer_listener_destroy(manager);
        return NULL;
    }
    if (!agent_peer_listener_reload(
            manager, peers, error, error_capacity)) {
        agent_peer_listener_destroy(manager);
        return NULL;
    }
    manager->timer.cb = timer_callback;
    uloop_timeout_set(&manager->timer, (int)config->session.tick_ms);
    return manager;
}

void agent_peer_listener_destroy(struct agent_peer_listener *manager)
{
    if (manager == NULL) {
        return;
    }
    uloop_timeout_cancel(&manager->timer);
    free_slots(manager);
    if (manager->listener.registered) {
        uloop_fd_delete(&manager->listener);
    }
    if (manager->listener.fd >= 0) {
        close(manager->listener.fd);
    }
    free_tls_global(manager);
    free(manager);
}

bool agent_peer_listener_get_status(
    const struct agent_peer_listener *manager,
    const char *peer_id,
    struct agent_peer_listener_status *status
)
{
    size_t index;

    if (manager == NULL || peer_id == NULL || status == NULL) {
        return false;
    }
    for (index = 0U; index < manager->slot_count; index++) {
        const struct listener_peer_slot *slot = &manager->slots[index];

        if (strcmp(slot->peer_id, peer_id) != 0) {
            continue;
        }
        memset(status, 0, sizeof(*status));
        status->configured = true;
        status->eligible = slot->eligible;
        status->session_up = slot->phase == LISTENER_PEER_ESTABLISHED;
        status->phase = listener_phase_name(slot->phase);
        status->direction = "inbound";
        status->local_sequence = slot->local_sequence;
        status->accepts = slot->accepts;
        status->tls_handshakes = slot->tls_handshakes;
        status->h2_sessions = slot->h2_sessions;
        status->messages_sent = slot->messages_sent;
        status->messages_received = slot->messages_received;
        status->protocol_errors = slot->protocol_errors +
                                  slot->session.protocol_errors;
        status->reconnects = slot->reconnects;
        status->invoke_tunnel_enabled =
            manager->config.session.relay_tunnel_enabled;
        status->invoke_tunnel_up = status->invoke_tunnel_enabled &&
            slot->connection != NULL &&
            slot->connection->tunnel_response_started &&
            !slot->connection->tunnel_stream_closed;
        if (slot->connection != NULL) {
            status->invoke_tunnel_streams =
                slot->connection->tunnel_mux.active_streams;
            status->invoke_tunnel_frames_sent =
                slot->connection->tunnel_mux.frames_sent;
            status->invoke_tunnel_frames_received =
                slot->connection->tunnel_mux.frames_received;
            status->invoke_tunnel_protocol_errors =
                slot->connection->tunnel_mux.protocol_errors;
        }
        (void)copy_text(status->last_error, sizeof(status->last_error),
                        slot->last_error);
        return true;
    }
    return false;
}

void agent_peer_listener_get_stats(
    const struct agent_peer_listener *manager,
    struct agent_peer_listener_stats *stats
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
    stats->listening = manager->listener.fd >= 0;
    (void)copy_text(stats->listen_ipv4, sizeof(stats->listen_ipv4),
                    manager->config.listen_ipv4);
    stats->listen_port = manager->config.listen_port;
    stats->active_connections = manager->active_connections;
    stats->accepts = manager->accepts;
    stats->tls_handshakes = manager->tls_handshakes;
    stats->h2_sessions = manager->h2_sessions;
    stats->messages_sent = manager->messages_sent;
    stats->messages_received = manager->messages_received;
    stats->failures = manager->failures;
    stats->rejected_connections = manager->rejected_connections;
    stats->invoke_tunnel_enabled =
        manager->config.session.relay_tunnel_enabled;
    for (index = 0U; index < manager->slot_count; index++) {
        if (manager->slots[index].eligible) {
            stats->eligible++;
        }
        if (manager->slots[index].phase == LISTENER_PEER_ESTABLISHED) {
            stats->sessions_up++;
        }
        if (manager->slots[index].connection != NULL) {
            const struct listener_connection *connection =
                manager->slots[index].connection;
            if (connection->tunnel_response_started &&
                !connection->tunnel_stream_closed) {
                stats->invoke_tunnels_up++;
            }
            stats->invoke_tunnel_streams +=
                connection->tunnel_mux.active_streams;
            stats->invoke_tunnel_frames_sent +=
                connection->tunnel_mux.frames_sent;
            stats->invoke_tunnel_frames_received +=
                connection->tunnel_mux.frames_received;
            stats->invoke_tunnel_protocol_errors +=
                connection->tunnel_mux.protocol_errors;
        }
    }
}

bool agent_peer_listener_send(
    struct agent_peer_listener *manager,
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
        struct listener_peer_slot *slot = &manager->slots[index];
        struct listener_connection *connection = slot->connection;

        if (strcmp(slot->peer_id, peer_id) != 0 ||
            slot->phase != LISTENER_PEER_ESTABLISHED ||
            connection == NULL || slot->local_sequence == UINT64_MAX) {
            continue;
        }
        outbound = *message;
        outbound.version = AGENT_ARPX_VERSION;
        if (!copy_text(outbound.router_id, sizeof(outbound.router_id),
                       manager->config.session.local_router_id) ||
            !copy_text(outbound.domain_id, sizeof(outbound.domain_id),
                       manager->config.session.local_domain_id)) {
            return false;
        }
        outbound.boot_epoch = manager->config.boot_epoch;
        outbound.sequence = slot->local_sequence + 1U;
        outbound.heartbeat_ms = 0U;
        if (agent_arpx_message_validate(&outbound) != AGENT_ARPX_OK ||
            !queue_payload(connection, &outbound)) {
            return false;
        }
        slot->local_sequence++;
        return update_interest(connection);
    }
    return false;
}

bool agent_peer_listener_tunnel_open(
    struct agent_peer_listener *manager,
    const char *peer_id,
    struct agent_relay_tunnel_message *message
)
{
    size_t index;

    if (manager == NULL || peer_id == NULL || message == NULL ||
        message->type != AGENT_RELAY_TUNNEL_OPEN) return false;
    for (index = 0U; index < manager->slot_count; index++) {
        struct listener_peer_slot *slot = &manager->slots[index];
        struct listener_connection *connection = slot->connection;

        if (strcmp(slot->peer_id, peer_id) != 0 ||
            slot->phase != LISTENER_PEER_ESTABLISHED ||
            connection == NULL || !connection->tunnel_response_started ||
            !tunnel_queue_available(connection)) continue;
        if (agent_relay_mux_open(&connection->tunnel_mux, message) !=
                AGENT_RELAY_TUNNEL_OK ||
            !encode_and_queue_tunnel(connection, message)) return false;
        return update_interest(connection);
    }
    return false;
}

bool agent_peer_listener_tunnel_send(
    struct agent_peer_listener *manager,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message
)
{
    size_t index;

    if (manager == NULL || peer_id == NULL || message == NULL ||
        message->type == AGENT_RELAY_TUNNEL_OPEN) return false;
    for (index = 0U; index < manager->slot_count; index++) {
        struct listener_peer_slot *slot = &manager->slots[index];
        struct listener_connection *connection = slot->connection;

        if (strcmp(slot->peer_id, peer_id) != 0 ||
            slot->phase != LISTENER_PEER_ESTABLISHED ||
            connection == NULL || !connection->tunnel_response_started ||
            !tunnel_queue_available(connection)) continue;
        if (agent_relay_tunnel_message_validate(message) !=
                AGENT_RELAY_TUNNEL_OK ||
            agent_relay_mux_on_send(&connection->tunnel_mux, message) !=
                AGENT_RELAY_TUNNEL_OK ||
            !encode_and_queue_tunnel(connection, message)) return false;
        return update_interest(connection);
    }
    return false;
}

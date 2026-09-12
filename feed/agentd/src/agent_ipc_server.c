#include "agent_ipc_server.h"

#include "agent_ipc_protocol.h"
#include "agent_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define AGENT_IPC_RUNTIME_PREFIX "/var/run/agentd/"

struct agent_ipc_client {
    struct uloop_fd uloop;
    struct agent_ipc_server *server;
    bool pending_invoke;
    bool stream_started;
};

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL) {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static void close_client(struct agent_ipc_client *client)
{
    if (client == NULL) {
        return;
    }
    uloop_fd_delete(&client->uloop);
    close(client->uloop.fd);
    free(client);
}

static void fill_relay_fields(
    const struct agent_ipc_server *server,
    const struct agent_route *route,
    bool *relay,
    char *relay_peer_id,
    size_t relay_peer_capacity,
    char *target_router_id,
    size_t target_router_capacity
)
{
    const struct agent_peer *peer;

    *relay = false;
    if (route->source != AGENT_ROUTE_SOURCE_PEER ||
        route->learned_from_peer[0] == '\0' || route->path_length == 0U) {
        return;
    }
    peer = peer_table_find(server->peers, route->learned_from_peer);
    if (peer == NULL || peer->state != AGENT_PEER_STATE_ESTABLISHED) {
        return;
    }
    if (!copy_text(relay_peer_id, relay_peer_capacity,
                   route->learned_from_peer) ||
        !copy_text(target_router_id, target_router_capacity,
                   route->path[0])) {
        relay_peer_id[0] = '\0';
        target_router_id[0] = '\0';
        return;
    }
    *relay = true;
}

static void send_error_frame(
    int fd,
    uint32_t request_id,
    uint32_t code,
    const char *message
)
{
    struct agent_ipc_error_response error_response;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length;

    memset(&error_response, 0, sizeof(error_response));
    error_response.request_id = request_id;
    error_response.code = code;
    if (!copy_text(error_response.message, sizeof(error_response.message),
                   message)) {
        return;
    }
    if (agent_ipc_encode_error_response(
            &error_response, frame, sizeof(frame),
            &frame_length) != AGENT_IPC_OK) {
        return;
    }
    (void)send(fd, frame, frame_length, MSG_NOSIGNAL);
}

static bool build_query(
    const struct agent_ipc_lookup_request *request,
    struct route_query *query
)
{
    memset(query, 0, sizeof(*query));
    if (!copy_text(query->intent, sizeof(query->intent), request->intent) ||
        !copy_text(query->tenant, sizeof(query->tenant), request->tenant) ||
        !copy_text(query->region, sizeof(query->region), request->region) ||
        (request->source_agent[0] != '\0' &&
         !copy_text(query->source_agent, sizeof(query->source_agent),
                    request->source_agent)) ||
        (request->target_agent[0] != '\0' &&
         !copy_text(query->target_agent, sizeof(query->target_agent),
                    request->target_agent))) {
        return false;
    }
    query->version = request->version;
    query->max_cost_microunits = request->max_cost_microunits;
    query->max_latency_ms = request->max_latency_ms;
    query->min_trust_level = request->min_trust_level;
    return true;
}

static bool lookup_selection(
    const struct agent_ipc_server *server,
    const struct agent_ipc_lookup_request *request,
    const struct route_query *query,
    struct route_selection *selection
)
{
    const struct agent_public_ipv6_lease *lease;

    if (request->public_ipv6[0] == '\0') {
        return route_table_lookup(server->routes, query, server->now_ms(),
                                  selection);
    }
    lease = agent_public_ipv6_find_address(server->public_ipv6,
                                           request->public_ipv6);
    /*
     * A Cloud registration publishes one /128 for an Agent origin, while the
     * origin may expose multiple capability routes.  Keep the /128 as an
     * origin boundary and let the authenticated target plus intent select the
     * matching local route; pinning the address to the lease's original route
     * ID makes every sibling tool unreachable.
     */
    if (lease == NULL || lease->expires_ms <= server->now_ms() ||
        query->target_agent[0] == '\0' ||
        strcmp(query->target_agent, lease->origin) != 0 ||
        !route_table_lookup(server->routes, query, server->now_ms(),
                            selection) ||
        selection->route->source != AGENT_ROUTE_SOURCE_LOCAL ||
        strcmp(selection->route->origin, lease->origin) != 0) {
        memset(selection, 0, sizeof(*selection));
        return false;
    }
    return true;
}

static void handle_single_lookup(
    struct agent_ipc_client *client,
    const uint8_t *frame,
    size_t frame_length
)
{
    struct agent_ipc_lookup_request request;
    struct agent_ipc_lookup_response response;
    struct route_query query;
    struct route_selection selection;
    uint8_t response_frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t response_length;
    bool found;
    struct agent_policy_admission admission;

    if (agent_ipc_decode_lookup_request(
            frame, frame_length, &request) != AGENT_IPC_OK) {
        send_error_frame(client->uloop.fd, 0U, 400U,
                         "invalid lookup frame");
        return;
    }

    if (!build_query(&request, &query)) {
        send_error_frame(client->uloop.fd, request.request_id, 400U,
                         "lookup field exceeds route table limit");
        return;
    }
    found = lookup_selection(client->server, &request, &query, &selection);
    memset(&response, 0, sizeof(response));
    response.request_id = request.request_id;
    response.found = found;
    response.generation = client->server->routes->generation;
    route_table_policy_admission(client->server->routes, &query, &admission);
    (void)copy_text(response.policy_id, sizeof(response.policy_id),
                    admission.policy_id);
    response.tenant_max_inflight = admission.max_inflight;
    response.tenant_rate_per_second = admission.rate_per_second;
    response.tenant_rate_burst = admission.rate_burst;
    if (found) {
        response.score = selection.score;
        if (!copy_text(response.route_id, sizeof(response.route_id),
                       selection.route->route_id) ||
            !copy_text(response.origin, sizeof(response.origin),
                       selection.route->origin) ||
            !copy_text(response.endpoint, sizeof(response.endpoint),
                       selection.route->endpoint) ||
            !copy_text(response.source, sizeof(response.source),
                       agent_route_source_name(selection.route->source))) {
            send_error_frame(client->uloop.fd, request.request_id, 500U,
                             "failed to encode route");
            return;
        }
        fill_relay_fields(client->server, selection.route,
                          &response.relay,
                          response.relay_peer_id,
                          sizeof(response.relay_peer_id),
                          response.target_router_id,
                          sizeof(response.target_router_id));
    }

    if (agent_ipc_encode_lookup_response(
            &response, response_frame, sizeof(response_frame),
            &response_length) != AGENT_IPC_OK) {
        send_error_frame(client->uloop.fd, request.request_id, 500U,
                         "failed to encode lookup response");
        return;
    }
    (void)send(client->uloop.fd, response_frame, response_length,
               MSG_NOSIGNAL);
}

static void handle_candidates_lookup(
    struct agent_ipc_client *client,
    const uint8_t *frame,
    size_t frame_length
)
{
    struct agent_ipc_candidates_request request;
    struct agent_ipc_candidates_response response;
    struct route_query query;
    struct route_selection selections[AGENT_IPC_MAX_CANDIDATES];
    uint8_t response_frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t response_length;
    size_t count;
    size_t index;
    struct agent_policy_admission admission;

    if (agent_ipc_decode_candidates_request(
            frame, frame_length, &request) != AGENT_IPC_OK) {
        send_error_frame(client->uloop.fd, 0U, 400U,
                         "invalid candidates lookup frame");
        return;
    }
    if (!build_query(&request.lookup, &query)) {
        send_error_frame(client->uloop.fd, request.lookup.request_id, 400U,
                         "lookup field exceeds route table limit");
        return;
    }
    memset(selections, 0, sizeof(selections));
    if (request.lookup.public_ipv6[0] != '\0') {
        count = lookup_selection(client->server, &request.lookup, &query,
                                 &selections[0]) ? 1U : 0U;
    } else {
        count = route_table_lookup_candidates_ex(
            client->server->routes, &query, client->server->now_ms(),
            selections, request.max_candidates, NULL);
    }
    memset(&response, 0, sizeof(response));
    response.request_id = request.lookup.request_id;
    response.generation = client->server->routes->generation;
    response.count = (uint8_t)count;
    route_table_policy_admission(client->server->routes, &query, &admission);
    (void)copy_text(response.policy_id, sizeof(response.policy_id),
                    admission.policy_id);
    response.tenant_max_inflight = admission.max_inflight;
    response.tenant_rate_per_second = admission.rate_per_second;
    response.tenant_rate_burst = admission.rate_burst;
    for (index = 0U; index < count; index++) {
        response.candidates[index].score = selections[index].score;
        if (!copy_text(response.candidates[index].route_id,
                       sizeof(response.candidates[index].route_id),
                       selections[index].route->route_id) ||
            !copy_text(response.candidates[index].origin,
                       sizeof(response.candidates[index].origin),
                       selections[index].route->origin) ||
            !copy_text(response.candidates[index].endpoint,
                       sizeof(response.candidates[index].endpoint),
                       selections[index].route->endpoint) ||
            !copy_text(response.candidates[index].source,
                       sizeof(response.candidates[index].source),
                       agent_route_source_name(
                           selections[index].route->source))) {
            send_error_frame(client->uloop.fd,
                             request.lookup.request_id, 500U,
                             "failed to encode route candidate");
            return;
        }
        fill_relay_fields(client->server, selections[index].route,
                          &response.candidates[index].relay,
                          response.candidates[index].relay_peer_id,
                          sizeof(response.candidates[index].relay_peer_id),
                          response.candidates[index].target_router_id,
                          sizeof(response.candidates[index].target_router_id));
    }
    if (agent_ipc_encode_candidates_response(
            &response, response_frame, sizeof(response_frame),
            &response_length) != AGENT_IPC_OK) {
        send_error_frame(client->uloop.fd, request.lookup.request_id, 500U,
                         "failed to encode candidates response");
        return;
    }
    (void)send(client->uloop.fd, response_frame, response_length,
               MSG_NOSIGNAL);
}

static void send_lease_response(
    struct agent_ipc_client *client,
    const struct agent_ipc_lease_response *response
)
{
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    if (agent_ipc_encode_lease_response(
            response, frame, sizeof(frame), &frame_length) != AGENT_IPC_OK ||
        send(client->uloop.fd, frame, frame_length, MSG_NOSIGNAL) !=
            (ssize_t)frame_length) {
        send_error_frame(client->uloop.fd, response->request_id, 500U,
                         "failed to encode lease response");
    }
}

static void handle_registration(
    struct agent_ipc_client *client,
    uint16_t type,
    const uint8_t *frame,
    size_t frame_length
)
{
    struct agent_ipc_register_request registration;
    struct agent_ipc_renew_request renewal;
    struct agent_ipc_unregister_request removal;
    struct agent_ipc_lease_response response;
    uint32_t request_id = 0U;
    uint32_t error_code = 400U;
    char error[AGENT_IPC_ERROR_LEN] = {0};
    bool handled = false;

    memset(&response, 0, sizeof(response));
    if (type == AGENT_IPC_REGISTER_REQUEST &&
        agent_ipc_decode_register_request(
            frame, frame_length, &registration) == AGENT_IPC_OK) {
        request_id = registration.request_id;
        if (client->server->register_handler != NULL)
            handled = client->server->register_handler(
                client->server->registration_context, &registration,
                &response, &error_code, error, sizeof(error));
    } else if (type == AGENT_IPC_RENEW_REQUEST &&
               agent_ipc_decode_renew_request(
                   frame, frame_length, &renewal) == AGENT_IPC_OK) {
        request_id = renewal.request_id;
        if (client->server->renew_handler != NULL)
            handled = client->server->renew_handler(
                client->server->registration_context, &renewal,
                &response, &error_code, error, sizeof(error));
    } else if (type == AGENT_IPC_UNREGISTER_REQUEST &&
               agent_ipc_decode_unregister_request(
                   frame, frame_length, &removal) == AGENT_IPC_OK) {
        request_id = removal.request_id;
        if (client->server->unregister_handler != NULL)
            handled = client->server->unregister_handler(
                client->server->registration_context, &removal,
                &response, &error_code, error, sizeof(error));
    } else {
        send_error_frame(client->uloop.fd, request_id, 400U,
                         "invalid registration frame");
        return;
    }
    if (!handled) {
        send_error_frame(client->uloop.fd, request_id,
                         error_code == 0U ? 500U : error_code,
                         error[0] == '\0' ?
                             "registration operation failed" : error);
        return;
    }
    send_lease_response(client, &response);
}

static bool handle_request(
    struct agent_ipc_client *client,
    const uint8_t *frame,
    size_t frame_length
)
{
    struct agent_ipc_header header;

    if (agent_ipc_decode_header(frame, frame_length, &header) !=
        AGENT_IPC_OK) {
        send_error_frame(client->uloop.fd, 0U, 400U,
                         "invalid IPC frame");
        return false;
    }
    if (header.type == AGENT_IPC_LOOKUP_REQUEST) {
        handle_single_lookup(client, frame, frame_length);
    } else if (header.type == AGENT_IPC_CANDIDATES_REQUEST) {
        handle_candidates_lookup(client, frame, frame_length);
    } else if (header.type == AGENT_IPC_INVOKE_REQUEST ||
               header.type == AGENT_IPC_STREAM_REQUEST) {
        struct agent_ipc_invoke_request request;

        enum agent_ipc_result decoded =
            header.type == AGENT_IPC_STREAM_REQUEST
                ? agent_ipc_decode_stream_request(
                      frame, frame_length, &request)
                : agent_ipc_decode_invoke_request(
                      frame, frame_length, &request);

        if (decoded != AGENT_IPC_OK) {
            send_error_frame(client->uloop.fd, header.request_id, 400U,
                             "invalid invoke frame");
        } else if (client->server->invoke_handler == NULL) {
            send_error_frame(client->uloop.fd, request.request_id, 503U,
                             "Relay invoke is unavailable");
        } else {
            client->pending_invoke = true;
            if (!client->server->invoke_handler(
                    client->server->invoke_context, client, &request)) {
                client->pending_invoke = false;
                send_error_frame(client->uloop.fd, request.request_id, 503U,
                                 "Relay invoke is unavailable");
            } else {
                client->server->invoke_requests++;
                return true;
            }
        }
    } else if (header.type == AGENT_IPC_REGISTER_REQUEST ||
               header.type == AGENT_IPC_RENEW_REQUEST ||
               header.type == AGENT_IPC_UNREGISTER_REQUEST) {
        handle_registration(client, header.type, frame, frame_length);
    } else {
        send_error_frame(client->uloop.fd, header.request_id, 400U,
                         "unsupported IPC request type");
    }
    return false;
}

static void client_readable(struct uloop_fd *uloop_fd, unsigned int events)
{
    struct agent_ipc_client *client =
        container_of(uloop_fd, struct agent_ipc_client, uloop);
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    ssize_t received;

    if ((events & ULOOP_READ) == 0U) {
        close_client(client);
        return;
    }

    received = recv(client->uloop.fd, frame, sizeof(frame), MSG_TRUNC);
    if (client->pending_invoke) {
        if (client->server->cancel_handler != NULL) {
            client->server->cancel_handler(
                client->server->invoke_context, client);
        }
        close_client(client);
        return;
    }
    if (received > 0 && (size_t)received <= sizeof(frame) &&
        handle_request(client, frame, (size_t)received)) {
        return;
    }
    close_client(client);
}

static void listener_readable(struct uloop_fd *uloop_fd, unsigned int events)
{
    struct agent_ipc_server *server =
        container_of(uloop_fd, struct agent_ipc_server, listener);
    struct agent_ipc_client *client;
    struct ucred credentials;
    socklen_t credentials_length;
    int client_fd;

    if ((events & ULOOP_READ) == 0U) {
        return;
    }

    for (;;) {
        client_fd = accept4(server->listener.fd, NULL, NULL,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        memset(&credentials, 0, sizeof(credentials));
        credentials_length = sizeof(credentials);
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
                       &credentials, &credentials_length) != 0 ||
            credentials_length != sizeof(credentials) ||
            credentials.uid != server->allowed_uid) {
            server->rejected_credentials++;
            close(client_fd);
            continue;
        }
        server->accepted_connections++;

        client = calloc(1U, sizeof(*client));
        if (client == NULL) {
            close(client_fd);
            continue;
        }
        client->server = server;
        client->uloop.fd = client_fd;
        client->uloop.cb = client_readable;
        if (uloop_fd_add(&client->uloop, ULOOP_READ) != 0) {
            close_client(client);
        }
    }
}

static bool socket_path_is_safe(const char *socket_path)
{
    return socket_path != NULL &&
           strncmp(socket_path, AGENT_IPC_RUNTIME_PREFIX,
                   strlen(AGENT_IPC_RUNTIME_PREFIX)) == 0 &&
           strstr(socket_path, "..") == NULL &&
           strlen(socket_path) < AGENT_IPC_SOCKET_PATH_LEN;
}

int agent_ipc_server_start(
    struct agent_ipc_server *server,
    const char *socket_path,
    struct route_table *routes,
    struct peer_table *peers,
    struct agent_public_ipv6_pool *public_ipv6,
    uint64_t (*now_ms)(void),
    uid_t allowed_uid,
    agent_ipc_invoke_handler invoke_handler,
    agent_ipc_cancel_handler cancel_handler,
    void *invoke_context,
    agent_ipc_register_handler register_handler,
    agent_ipc_renew_handler renew_handler,
    agent_ipc_unregister_handler unregister_handler,
    void *registration_context
)
{
    struct sockaddr_un address;
    struct stat existing;
    int fd;

    if (server == NULL ||
        routes == NULL || peers == NULL || public_ipv6 == NULL ||
        now_ms == NULL ||
        !socket_path_is_safe(socket_path)) {
        errno = EINVAL;
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->listener.fd = -1;
    if (lstat(socket_path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || unlink(socket_path) != 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (!copy_text(address.sun_path, sizeof(address.sun_path), socket_path) ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, 0660) != 0 ||
        listen(fd, 16) != 0) {
        close(fd);
        unlink(socket_path);
        return -1;
    }

    server->listener.fd = fd;
    server->listener.cb = listener_readable;
    server->routes = routes;
    server->peers = peers;
    server->public_ipv6 = public_ipv6;
    server->now_ms = now_ms;
    server->allowed_uid = allowed_uid;
    server->invoke_handler = invoke_handler;
    server->cancel_handler = cancel_handler;
    server->invoke_context = invoke_context;
    server->register_handler = register_handler;
    server->renew_handler = renew_handler;
    server->unregister_handler = unregister_handler;
    server->registration_context = registration_context;
    copy_text(server->socket_path, sizeof(server->socket_path), socket_path);
    if (uloop_fd_add(&server->listener, ULOOP_READ) != 0) {
        agent_ipc_server_stop(server);
        return -1;
    }
    return 0;
}

bool agent_ipc_client_respond_invoke(
    struct agent_ipc_client *client,
    const struct agent_ipc_invoke_response *response
)
{
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;
    bool sent;

    if (client == NULL || !client->pending_invoke ||
        client->stream_started || response == NULL ||
        agent_ipc_encode_invoke_response(
            response, frame, sizeof(frame), &frame_length) != AGENT_IPC_OK) {
        return false;
    }
    sent = send(client->uloop.fd, frame, frame_length, MSG_NOSIGNAL) ==
           (ssize_t)frame_length;
    if (sent) client->server->invoke_completions++;
    client->pending_invoke = false;
    close_client(client);
    return sent;
}

bool agent_ipc_client_respond_error(
    struct agent_ipc_client *client,
    uint32_t request_id,
    uint32_t code,
    const char *message
)
{
    if (client == NULL || !client->pending_invoke) return false;
    send_error_frame(client->uloop.fd, request_id, code, message);
    client->pending_invoke = false;
    close_client(client);
    return true;
}

bool agent_ipc_client_stream_start(
    struct agent_ipc_client *client,
    uint32_t request_id,
    uint16_t status_code,
    const char *content_type
)
{
    struct agent_ipc_stream_start start;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    if (client == NULL || !client->pending_invoke ||
        client->stream_started || content_type == NULL) return false;
    memset(&start, 0, sizeof(start));
    start.request_id = request_id;
    start.status_code = status_code;
    if (!copy_text(start.content_type, sizeof(start.content_type),
                   content_type) ||
        agent_ipc_encode_stream_start(
            &start, frame, sizeof(frame), &frame_length) != AGENT_IPC_OK ||
        send(client->uloop.fd, frame, frame_length, MSG_NOSIGNAL) !=
            (ssize_t)frame_length) return false;
    client->stream_started = true;
    client->server->stream_starts++;
    return true;
}

bool agent_ipc_client_stream_data(
    struct agent_ipc_client *client,
    uint32_t request_id,
    const uint8_t *data,
    size_t data_length
)
{
    struct agent_ipc_stream_data stream_data;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;

    if (client == NULL || !client->pending_invoke ||
        !client->stream_started || data == NULL || data_length == 0U ||
        data_length > sizeof(stream_data.data)) return false;
    memset(&stream_data, 0, sizeof(stream_data));
    stream_data.request_id = request_id;
    stream_data.data_length = data_length;
    memcpy(stream_data.data, data, data_length);
    if (agent_ipc_encode_stream_data(
            &stream_data, frame, sizeof(frame), &frame_length) != AGENT_IPC_OK ||
        send(client->uloop.fd, frame, frame_length, MSG_NOSIGNAL) !=
            (ssize_t)frame_length) return false;
    client->server->stream_data_frames++;
    return true;
}

bool agent_ipc_client_stream_end(
    struct agent_ipc_client *client,
    uint32_t request_id,
    uint64_t total_bytes
)
{
    struct agent_ipc_stream_end end;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length = 0U;
    bool sent;

    if (client == NULL || !client->pending_invoke ||
        !client->stream_started) return false;
    memset(&end, 0, sizeof(end));
    end.request_id = request_id;
    end.total_bytes = total_bytes;
    if (agent_ipc_encode_stream_end(
            &end, frame, sizeof(frame), &frame_length) != AGENT_IPC_OK) {
        return false;
    }
    sent = send(client->uloop.fd, frame, frame_length, MSG_NOSIGNAL) ==
           (ssize_t)frame_length;
    if (sent) {
        client->server->stream_completions++;
        client->server->invoke_completions++;
    }
    client->pending_invoke = false;
    close_client(client);
    return sent;
}

void agent_ipc_server_stop(struct agent_ipc_server *server)
{
    struct stat existing;

    if (server == NULL) {
        return;
    }
    if (server->listener.fd >= 0) {
        uloop_fd_delete(&server->listener);
        close(server->listener.fd);
        server->listener.fd = -1;
    }
    if (server->socket_path[0] != '\0' &&
        lstat(server->socket_path, &existing) == 0 &&
        S_ISSOCK(existing.st_mode)) {
        unlink(server->socket_path);
    }
    server->socket_path[0] = '\0';
}

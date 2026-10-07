#include "adapter_codec.h"
#include "adapter_registry.h"
#include "agent_adapter_ingress_contract.h"
#include "agent_public_ingress.h"
#include "agent_invoke_contract.h"
#include "agent_sse_contract.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <getopt.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uhttpd/uhttpd.h>

#define ADAPTERD_DEFAULT_LISTEN "127.0.0.1:7790"
#define ADAPTERD_DEFAULT_GATEWAY \
    "http://127.0.0.1:7788/agent/v1/invoke"
#define ADAPTERD_DEFAULT_UCI_PACKAGE "agent_adapter"
#define ADAPTERD_DEFAULT_MANIFEST_FILE \
    "/var/run/agent-manifests/manifests.json"
#define ADAPTERD_MAX_MANIFEST_BYTES 1048576U
#define ADAPTERD_DEFAULT_MAX_REQUEST 65536U
#define ADAPTERD_DEFAULT_MAX_RESPONSE 262144U
#define ADAPTERD_DEFAULT_TIMEOUT_MS 5500U
#define ADAPTERD_DEFAULT_INTERACTIVE_TIMEOUT_MS 3600000U
#define ADAPTERD_DEFAULT_MAX_INFLIGHT 32U
#define ADAPTERD_DEFAULT_STREAM_IDLE_TIMEOUT_MS 15000U
#define ADAPTERD_DEFAULT_MAX_STREAM_EVENT 65536U
#define ADAPTERD_MAX_REQUEST_LIMIT 1048576U
#define ADAPTERD_MAX_RESPONSE_LIMIT 4194304U
#define ADAPTERD_MAX_HTTP_HEADER 8192U
#define ADAPTERD_LISTEN_LEN 128U
#define ADAPTERD_PATH_LEN 256U
#define ADAPTERD_IDENTITY_LEN 128U
#define ADAPTERD_REGION_LEN 64U
#define ADAPTERD_CONTEXT_URL_LEN 2048U
#define ADAPTERD_CONTEXT_TOKEN_LEN 256U
#define CONNECTION_REJECTED ((void *)(uintptr_t)1U)

struct adapterd_config {
    char listen[ADAPTERD_LISTEN_LEN];
    char gateway_url[ADAPTERD_PATH_LEN];
    char uci_package[64U];
    char uci_dir[ADAPTERD_PATH_LEN];
    char manifest_file[ADAPTERD_PATH_LEN];
    char tenant[ADAPTERD_IDENTITY_LEN];
    char source_agent[ADAPTERD_IDENTITY_LEN];
    char region[ADAPTERD_REGION_LEN];
    uint32_t hop_limit;
    uint32_t max_request_bytes;
    uint32_t max_response_bytes;
    uint32_t gateway_timeout_ms;
    uint32_t interactive_timeout_ms;
    uint32_t max_inflight;
    bool stream_enabled;
    uint32_t stream_idle_timeout_ms;
    uint32_t max_stream_event_bytes;
    struct agent_invoke_endpoint gateway;
};

struct adapterd_runtime {
    uint64_t active_requests;
    uint64_t admitted_requests;
    uint64_t completed_requests;
    uint64_t invalid_requests;
    uint64_t mapping_misses;
    uint64_t gateway_failures;
    uint64_t gateway_timeouts;
    uint64_t client_disconnects;
    uint64_t streams_started;
    uint64_t streams_completed;
    uint64_t stream_failures;
    uint64_t stream_events;
    uint64_t stream_bytes;
    uint64_t manifest_generation;
    uint64_t dynamic_mappings;
    uint64_t identical_mappings;
    uint64_t mapping_conflicts;
    uint64_t invalid_dynamic_mappings;
    uint64_t dynamic_capacity_rejections;
    uint64_t manifest_refresh_failures;
};

struct adapterd_request {
    struct uh_connection *connection;
    bool counted;
    bool retained;
    bool backend_started;
    bool stream_head_sent;
    bool stream_head_parsed;
    bool mcp_envelope_stream;
    size_t stream_output_bytes;
    struct adapter_mcp_stream_state mcp_stream;
    int backend_fd;
    struct agent_adapter_ingress_route route;
    struct adapter_normalized_request normalized;
    struct agent_invoke_backend_machine backend_machine;
    uint32_t effective_gateway_timeout_ms;
    ev_io backend_watcher;
    ev_timer deadline_watcher;
    char *backend_request;
    size_t backend_request_length;
    size_t backend_request_sent;
    char *backend_response;
    size_t backend_response_length;
    size_t backend_response_capacity;
    char *stream_event_buffer;
    struct agent_sse_relay stream_relay;
};

static struct adapterd_config config;
static struct adapterd_runtime runtime;
static struct adapter_registry registry;
static struct adapter_registry static_registry;
static ev_timer manifest_refresh_timer;

struct adapterd_manifest_snapshot {
    struct agent_adapter_mapping mappings[AGENT_ADAPTER_MAX_MAPPINGS];
    size_t count;
    uint64_t generation;
    bool valid;
};

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written > 0 && (size_t)written < capacity;
}

static bool copy_optional_json(
    char *target,
    size_t capacity,
    struct json_object *object,
    const char *name
)
{
    struct json_object *value;
    int written;

    if (target == NULL || capacity == 0U) return false;
    target[0] = '\0';
    if (!json_object_object_get_ex(object, name, &value)) return true;
    if (!json_object_is_type(value, json_type_string)) return false;
    written = snprintf(target, capacity, "%s", json_object_get_string(value));
    return written >= 0 && (size_t)written < capacity;
}

static bool required_json_text(
    struct json_object *object,
    const char *name,
    char *target,
    size_t capacity
)
{
    struct json_object *value;

    return json_object_object_get_ex(object, name, &value) &&
           json_object_is_type(value, json_type_string) &&
           copy_text(target, capacity, json_object_get_string(value));
}

static bool optional_json_bool(
    struct json_object *object,
    const char *name,
    bool *target
)
{
    struct json_object *value;

    *target = false;
    if (!json_object_object_get_ex(object, name, &value)) return true;
    if (!json_object_is_type(value, json_type_boolean)) return false;
    *target = json_object_get_boolean(value);
    return true;
}

static bool parse_dynamic_tool(
    struct json_object *tool,
    struct agent_adapter_mapping *mapping
)
{
    struct json_object *value;
    int64_t version;

    if (!json_object_is_type(tool, json_type_object) ||
        !json_object_object_get_ex(tool, "intent_version", &value) ||
        !json_object_is_type(value, json_type_int) ||
        (version = json_object_get_int64(value)) <= 0 ||
        (uint64_t)version > UINT32_MAX) return false;
    memset(mapping, 0, sizeof(*mapping));
    mapping->enabled = true;
    mapping->intent_version = (uint32_t)version;
    if (!json_object_object_get_ex(tool, "protocol", &value) ||
        !json_object_is_type(value, json_type_string)) return false;
    mapping->protocol = agent_adapter_protocol_parse(
        json_object_get_string(value));
    return required_json_text(tool, "authority", mapping->authority,
                              sizeof(mapping->authority)) &&
        required_json_text(tool, "selector", mapping->selector,
                           sizeof(mapping->selector)) &&
        required_json_text(tool, "intent", mapping->intent,
                           sizeof(mapping->intent)) &&
        required_json_text(tool, "input_schema_json",
                           mapping->input_schema_json,
                           sizeof(mapping->input_schema_json)) &&
        copy_optional_json(mapping->title, sizeof(mapping->title),
                           tool, "title") &&
        copy_optional_json(mapping->description,
                           sizeof(mapping->description),
                           tool, "description") &&
        optional_json_bool(tool, "task", &mapping->task) &&
        optional_json_bool(tool, "resumable", &mapping->resumable) &&
        optional_json_bool(tool, "demo", &mapping->demo) &&
        optional_json_bool(tool, "chat", &mapping->chat) &&
        optional_json_bool(tool, "interactive", &mapping->interactive);
}

static bool refresh_dynamic_registry(void)
{
    struct adapterd_manifest_snapshot snapshot;
    struct adapter_registry effective;
    struct adapter_registry_merge_result merge;
    struct json_tokener *tokener = NULL;
    struct json_object *root = NULL;
    struct json_object *items;
    struct json_object *value;
    struct stat status;
    char *body = NULL;
    size_t used = 0U;
    size_t index;
    int descriptor = -1;
    bool success = false;

    memset(&snapshot, 0, sizeof(snapshot));
    descriptor = open(config.manifest_file,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (uint64_t)status.st_size > ADAPTERD_MAX_MANIFEST_BYTES) goto done;
    body = malloc((size_t)status.st_size + 1U);
    if (body == NULL) goto done;
    while (used < (size_t)status.st_size) {
        ssize_t count = read(descriptor, body + used,
                             (size_t)status.st_size - used);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        goto done;
    }
    body[used] = '\0';
    tokener = json_tokener_new_ex(32);
    if (tokener == NULL) goto done;
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tokener, body, (int)used);
    if (root == NULL || json_tokener_get_error(tokener) != json_tokener_success ||
        json_tokener_get_parse_end(tokener) != used ||
        !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "generation", &value) ||
        !json_object_is_type(value, json_type_int) ||
        json_object_get_int64(value) < 0 ||
        !json_object_object_get_ex(root, "manifests", &items) ||
        !json_object_is_type(items, json_type_array) ||
        json_object_array_length(items) > AGENT_ADAPTER_MAX_MAPPINGS)
        goto done;
    snapshot.generation = (uint64_t)json_object_get_int64(value);
    snapshot.count = json_object_array_length(items);
    for (index = 0U; index < snapshot.count; index++) {
        struct json_object *item = json_object_array_get_idx(items, index);
        struct json_object *tool;

        if (item == NULL || !json_object_is_type(item, json_type_object) ||
            !json_object_object_get_ex(item, "tool", &tool) ||
            !parse_dynamic_tool(tool, &snapshot.mappings[index])) goto done;
    }
    snapshot.valid = true;
    if (!adapter_registry_merge(&static_registry, snapshot.mappings,
                                snapshot.count, &effective, &merge)) goto done;
    registry = effective;
    runtime.manifest_generation = snapshot.generation;
    runtime.dynamic_mappings = merge.dynamic_added;
    runtime.identical_mappings = merge.identical_merged;
    runtime.mapping_conflicts = merge.static_conflicts;
    runtime.invalid_dynamic_mappings = merge.invalid_dynamic;
    runtime.dynamic_capacity_rejections = merge.capacity_rejected;
    success = true;

done:
    if (descriptor >= 0) close(descriptor);
    if (root != NULL) json_object_put(root);
    if (tokener != NULL) json_tokener_free(tokener);
    free(body);
    if (!success) runtime.manifest_refresh_failures++;
    return success;
}

static void manifest_refresh_callback(
    struct ev_loop *loop,
    ev_timer *watcher,
    int revents
)
{
    (void)loop;
    (void)watcher;
    (void)revents;
    (void)refresh_dynamic_registry();
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool listen_is_loopback(const char *listen)
{
    const char *port;
    uint32_t port_number;

    if (listen == NULL) {
        return false;
    }
    if (strncmp(listen, "127.0.0.1:", 10U) == 0) {
        port = listen + 10U;
    } else if (strncmp(listen, "[::1]:", 6U) == 0) {
        port = listen + 6U;
    } else {
        return false;
    }
    return parse_u32(port, &port_number) &&
           port_number > 0U && port_number <= 65535U;
}

static bool content_type_allowed(
    struct uh_connection *connection,
    enum agent_adapter_protocol protocol
)
{
    struct uh_str value = connection->get_header(connection, "Content-Type");
    static const char json[] = "application/json";
    static const char a2a[] = "application/a2a+json";

    if (value.p == NULL) {
        return false;
    }
    if (protocol == AGENT_ADAPTER_PROTOCOL_A2A &&
        value.len >= sizeof(a2a) - 1U &&
        strncasecmp(value.p, a2a, sizeof(a2a) - 1U) == 0 &&
        (value.len == sizeof(a2a) - 1U || value.p[sizeof(a2a) - 1U] == ';')) {
        return true;
    }
    return value.len >= sizeof(json) - 1U &&
           strncasecmp(value.p, json, sizeof(json) - 1U) == 0 &&
           (value.len == sizeof(json) - 1U ||
            value.p[sizeof(json) - 1U] == ';');
}

static void send_json(
    struct uh_connection *connection,
    int status,
    const char *content_type,
    struct json_object *body
)
{
    const char *serialized = json_object_to_json_string_ext(
        body, JSON_C_TO_STRING_PLAIN);
    size_t length = strlen(serialized);

    connection->send_head(connection, status, (int64_t)length, NULL);
    connection->send_header(connection, "Content-Type", "%s", content_type);
    connection->send_header(connection, "Cache-Control", "no-store");
    connection->send_header(connection, "X-Content-Type-Options", "nosniff");
    connection->end_headers(connection);
    connection->send(connection, serialized, length);
    connection->end_response(connection);
}

static void send_empty(struct uh_connection *connection, int status)
{
    connection->send_head(connection, status, 0, NULL);
    connection->send_header(connection, "Cache-Control", "no-store");
    connection->send_header(connection, "X-Content-Type-Options", "nosniff");
    connection->end_headers(connection);
    connection->end_response(connection);
}

static void send_plain_error(
    struct uh_connection *connection,
    int status,
    const char *code,
    const char *message
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *error = json_object_new_object();

    if (root == NULL || error == NULL) {
        json_object_put(root);
        json_object_put(error);
        connection->send_error(connection, 500, "OUT_OF_MEMORY");
        return;
    }
    json_object_object_add(error, "code", json_object_new_string(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(root, "error", error);
    send_json(connection, status, "application/json; charset=utf-8", root);
    json_object_put(root);
}

static void destroy_request(struct adapterd_request *state)
{
    struct uh_connection *connection;
    struct ev_loop *loop;
    bool retained;

    if (state == NULL) {
        return;
    }
    connection = state->connection;
    retained = state->retained;
    loop = connection != NULL ? connection->get_loop(connection) : NULL;
    if (loop != NULL) {
        if (ev_is_active(&state->backend_watcher)) {
            ev_io_stop(loop, &state->backend_watcher);
        }
        if (ev_is_active(&state->deadline_watcher)) {
            ev_timer_stop(loop, &state->deadline_watcher);
        }
    }
    if (state->backend_fd >= 0) {
        close(state->backend_fd);
    }
    if (state->counted && runtime.active_requests > 0U) {
        runtime.active_requests--;
    }
    if (connection != NULL && connection->userdata == state) {
        connection->userdata = NULL;
    }
    adapter_codec_request_clear(&state->normalized);
    free(state->backend_request);
    free(state->backend_response);
    free(state->stream_event_buffer);
    free(state);
    if (retained) {
        connection->decref(connection);
    }
}

static void release_request(struct uh_connection *connection)
{
    if (connection->userdata == NULL ||
        connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    destroy_request(connection->userdata);
}

static void reject_head(
    struct uh_connection *connection,
    int status,
    const char *code
)
{
    if (status == 413 && strcmp(code, "REQUEST_TOO_LARGE") == 0) {
        char message[192];
        snprintf(message, sizeof(message),
                 "MCP/A2A request must be non-empty and at most %u bytes; "
                 "reduce inline data or use a file reference.", config.max_request_bytes);
        send_plain_error(connection, status, code, message);
    } else {
        connection->send_error(connection, status, "%s", code);
    }
    release_request(connection);
    connection->userdata = CONNECTION_REJECTED;
}

static void connection_closed(struct uh_connection *connection)
{
    struct adapterd_request *state;

    if (connection->userdata == NULL ||
        connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    state = connection->userdata;
    runtime.client_disconnects++;
    if (state->backend_started) {
        (void)agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_CLIENT_DISCONNECTED);
    }
    destroy_request(state);
}

static void send_mapped_gateway_response(
    struct adapterd_request *state,
    int gateway_status,
    const char *content_type,
    const char *body,
    size_t body_length
)
{
    struct json_object *mapped = adapter_codec_map_response(
        &state->normalized, gateway_status, content_type, body, body_length);

    if (mapped == NULL) {
        send_plain_error(state->connection, 502,
                         "RESPONSE_MAPPING_FAILED",
                         "gateway response could not be mapped");
    } else {
        send_json(state->connection, gateway_status,
                  adapter_codec_response_content_type(
                      state->normalized.protocol), mapped);
        json_object_put(mapped);
    }
    if (gateway_status >= 200 && gateway_status < 300) {
        runtime.completed_requests++;
    } else {
        runtime.gateway_failures++;
    }
    destroy_request(state);
}

static void fail_gateway(
    struct adapterd_request *state,
    int status,
    const char *code
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *error = json_object_new_object();
    const char *serialized;

    if (root == NULL || error == NULL) {
        json_object_put(root);
        json_object_put(error);
        send_plain_error(state->connection, 500, "OUT_OF_MEMORY",
                         "failed to allocate gateway error");
        runtime.gateway_failures++;
        destroy_request(state);
        return;
    }
    json_object_object_add(error, "code", json_object_new_string(code));
    json_object_object_add(root, "error", error);
    serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    send_mapped_gateway_response(state, status, "application/json",
                                 serialized, strlen(serialized));
    json_object_put(root);
}

static void send_protocol_failure(
    struct adapterd_request *state,
    int status,
    const char *code
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *error = json_object_new_object();
    struct json_object *mapped;
    const char *serialized;

    if (root == NULL || error == NULL) {
        json_object_put(root);
        json_object_put(error);
        send_plain_error(state->connection, 500, "OUT_OF_MEMORY",
                         "failed to allocate protocol error");
        destroy_request(state);
        return;
    }
    json_object_object_add(error, "code", json_object_new_string(code));
    json_object_object_add(root, "error", error);
    serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    mapped = adapter_codec_map_response(
        &state->normalized, status, "application/json",
        serialized, strlen(serialized));
    if (mapped == NULL) {
        send_plain_error(state->connection, status, code,
                         "protocol request could not be routed");
    } else {
        send_json(state->connection, status,
                  adapter_codec_response_content_type(
                      state->normalized.protocol), mapped);
        json_object_put(mapped);
    }
    json_object_put(root);
    destroy_request(state);
}

static void set_backend_events(struct adapterd_request *state, int events)
{
    struct ev_loop *loop = state->connection->get_loop(state->connection);

    if (ev_is_active(&state->backend_watcher)) {
        ev_io_stop(loop, &state->backend_watcher);
    }
    ev_io_set(&state->backend_watcher, state->backend_fd, events);
    ev_io_start(loop, &state->backend_watcher);
}

static bool try_send_backend(struct adapterd_request *state)
{
    size_t remaining = state->backend_request_length -
        state->backend_request_sent;
    ssize_t sent = send(state->backend_fd,
                        state->backend_request + state->backend_request_sent,
                        remaining, MSG_NOSIGNAL);

    if (sent > 0) {
        state->backend_request_sent += (size_t)sent;
        if (state->backend_request_sent == state->backend_request_length) {
            if (!agent_invoke_backend_machine_transition(
                    &state->backend_machine, AGENT_BACKEND_REQUEST_SENT)) {
                return false;
            }
            set_backend_events(state, EV_READ);
        }
        return true;
    }
    return sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                        errno == EINTR);
}

static void fail_stream(struct adapterd_request *state)
{
    runtime.gateway_failures++;
    runtime.stream_failures++;
    (void)agent_invoke_backend_machine_transition(
        &state->backend_machine, AGENT_BACKEND_IO_FAILED);
    if (state->stream_head_sent &&
        !state->connection->closed(state->connection)) {
        state->connection->userdata = NULL;
        state->connection->close(state->connection);
    }
    destroy_request(state);
}

static void reset_stream_idle_timer(struct adapterd_request *state)
{
    struct ev_loop *loop = state->connection->get_loop(state->connection);
    ev_tstamp timeout =
        (ev_tstamp)config.stream_idle_timeout_ms / 1000.0;

    if (ev_is_active(&state->deadline_watcher)) {
        ev_timer_stop(loop, &state->deadline_watcher);
    }
    ev_timer_set(&state->deadline_watcher, timeout, 0.0);
    ev_timer_start(loop, &state->deadline_watcher);
}

static bool emit_stream_event(
    const char *event,
    size_t event_length,
    void *context
)
{
    struct adapterd_request *state = context;
    struct json_object *message = NULL;
    char *mapped = NULL;
    bool sent = false;

    if (state->route.protocol == AGENT_ADAPTER_PROTOCOL_MCP &&
        !state->mcp_envelope_stream) {
        const char *json;
        size_t length;
        if (!adapter_codec_mcp_event(&state->normalized, &state->mcp_stream,
                                     event, event_length, &message)) return false;
        if (message == NULL) return true;
        json = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
        length = strlen(json);
        mapped = malloc(length + sizeof("event: message\ndata: \n\n"));
        if (mapped == NULL) { json_object_put(message); return false; }
        event_length = (size_t)sprintf(mapped, "event: message\ndata: %s\n\n", json);
        event = mapped;
    }

    if (state->connection->closed(state->connection) ||
        state->stream_output_bytes > config.max_response_bytes ||
        event_length > config.max_response_bytes -
            state->stream_output_bytes) goto done;
    state->connection->send(state->connection, event, event_length);
    state->stream_output_bytes += event_length;
    runtime.stream_events++;
    runtime.stream_bytes += event_length;
    sent = true;
done:
    json_object_put(message);
    free(mapped);
    return sent;
}

static void complete_stream(struct adapterd_request *state)
{
    if (state->route.protocol == AGENT_ADAPTER_PROTOCOL_MCP &&
        !state->mcp_envelope_stream && !state->mcp_stream.finished) {
        fail_stream(state);
        return;
    }
    (void)agent_invoke_backend_machine_transition(
        &state->backend_machine, AGENT_BACKEND_RESPONSE_RECEIVED);
    runtime.completed_requests++;
    runtime.streams_completed++;
    if (!state->connection->closed(state->connection)) {
        state->connection->end_response(state->connection);
    }
    destroy_request(state);
}

static void handle_stream_read(struct adapterd_request *state)
{
    struct agent_sse_response_head head;
    struct agent_invoke_http_response response;
    enum agent_sse_head_result head_result;
    enum agent_sse_relay_result relay_result;
    enum agent_invoke_http_result invoke_result;
    char *destination;
    size_t capacity;
    ssize_t received;

    if (state->stream_head_parsed) {
        destination = state->backend_response;
        capacity = state->backend_response_capacity - 1U;
    } else {
        if (state->backend_response_length + 1U >=
            state->backend_response_capacity) {
            fail_gateway(state, 502, "GATEWAY_RESPONSE_TOO_LARGE");
            return;
        }
        destination = state->backend_response + state->backend_response_length;
        capacity = state->backend_response_capacity -
            state->backend_response_length - 1U;
    }
    received = recv(state->backend_fd, destination, capacity, 0);
    if (received < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (received < 0) {
        if (state->stream_head_sent) fail_stream(state);
        else fail_gateway(state, 502, "GATEWAY_UNAVAILABLE");
        return;
    }
    if (received > 0) reset_stream_idle_timer(state);
    if (!state->stream_head_parsed) {
        state->backend_response_length += received > 0 ? (size_t)received : 0U;
        state->backend_response[state->backend_response_length] = '\0';
        head_result = agent_sse_parse_response_head(
            state->backend_response, state->backend_response_length, &head);
        if (head_result == AGENT_SSE_HEAD_INCOMPLETE && received > 0) return;
        if (head_result != AGENT_SSE_HEAD_OK ||
            !agent_sse_response_is_streamable(&head)) {
            invoke_result = agent_invoke_parse_http_response(
                state->backend_response, state->backend_response_length,
                config.max_response_bytes, &response);
            if (invoke_result == AGENT_INVOKE_HTTP_INCOMPLETE && received > 0) {
                return;
            }
            if (invoke_result == AGENT_INVOKE_HTTP_OK) {
                send_mapped_gateway_response(
                    state, response.status, response.content_type,
                    state->backend_response + response.header_length,
                    response.body_length);
            } else {
                fail_gateway(state, 502, "GATEWAY_PROTOCOL_ERROR");
            }
            return;
        }
        agent_sse_relay_init(
            &state->stream_relay, head.chunked,
            state->stream_event_buffer, config.max_stream_event_bytes);
        state->stream_head_parsed = true;
        state->connection->send_head(state->connection, 200, -1, NULL);
        state->connection->send_header(
            state->connection, "Content-Type", "text/event-stream");
        state->connection->send_header(
            state->connection, "Cache-Control", "no-store");
        state->connection->send_header(
            state->connection, "X-Accel-Buffering", "no");
        state->connection->end_headers(state->connection);
        state->stream_head_sent = true;
        runtime.streams_started++;
        destination = state->backend_response + head.header_length;
        capacity = state->backend_response_length - head.header_length;
    } else {
        capacity = received > 0 ? (size_t)received : 0U;
    }
    if (capacity > 0U) {
        relay_result = agent_sse_relay_feed(
            &state->stream_relay, destination, capacity,
            emit_stream_event, state);
        if (relay_result == AGENT_SSE_RELAY_DONE) {
            complete_stream(state);
            return;
        }
        if (relay_result != AGENT_SSE_RELAY_OK) {
            fail_stream(state);
            return;
        }
    }
    state->backend_response_length = 0U;
    if (received == 0) {
        relay_result = agent_sse_relay_finish(&state->stream_relay);
        if (relay_result == AGENT_SSE_RELAY_DONE) complete_stream(state);
        else fail_stream(state);
    }
}

static void backend_event_callback(
    struct ev_loop *loop,
    ev_io *watcher,
    int revents
)
{
    struct adapterd_request *state = watcher->data;
    struct agent_invoke_http_response response;
    enum agent_invoke_http_result parse_result;
    ssize_t received;

    (void)loop;
    if ((revents & EV_ERROR) != 0) {
        (void)agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_IO_FAILED);
        fail_gateway(state, 502, "GATEWAY_UNAVAILABLE");
        return;
    }
    if (state->backend_machine.phase == AGENT_BACKEND_CONNECTING &&
        (revents & EV_WRITE) != 0) {
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);

        if (getsockopt(state->backend_fd, SOL_SOCKET, SO_ERROR,
                       &socket_error, &length) != 0 || socket_error != 0 ||
            !agent_invoke_backend_machine_transition(
                &state->backend_machine, AGENT_BACKEND_CONNECTED) ||
            !try_send_backend(state)) {
            (void)agent_invoke_backend_machine_transition(
                &state->backend_machine, AGENT_BACKEND_IO_FAILED);
            fail_gateway(state, 502, "GATEWAY_UNAVAILABLE");
        }
        return;
    }
    if (state->backend_machine.phase == AGENT_BACKEND_SENDING &&
        (revents & EV_WRITE) != 0) {
        if (!try_send_backend(state)) {
            (void)agent_invoke_backend_machine_transition(
                &state->backend_machine, AGENT_BACKEND_IO_FAILED);
            fail_gateway(state, 502, "GATEWAY_UNAVAILABLE");
        }
        return;
    }
    if (state->backend_machine.phase != AGENT_BACKEND_RECEIVING ||
        (revents & EV_READ) == 0) {
        (void)agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_IO_FAILED);
        fail_gateway(state, 502, "GATEWAY_PROTOCOL_ERROR");
        return;
    }
    if (state->route.streaming) {
        handle_stream_read(state);
        return;
    }
    if (state->backend_response_length + 1U >=
        state->backend_response_capacity) {
        fail_gateway(state, 502, "GATEWAY_RESPONSE_TOO_LARGE");
        return;
    }
    received = recv(
        state->backend_fd,
        state->backend_response + state->backend_response_length,
        state->backend_response_capacity - state->backend_response_length - 1U,
        0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                         errno == EINTR)) {
        return;
    }
    if (received < 0) {
        (void)agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_IO_FAILED);
        fail_gateway(state, 502, "GATEWAY_UNAVAILABLE");
        return;
    }
    if (received > 0) {
        state->backend_response_length += (size_t)received;
        state->backend_response[state->backend_response_length] = '\0';
    }
    parse_result = agent_invoke_parse_http_response(
        state->backend_response, state->backend_response_length,
        config.max_response_bytes, &response);
    if (parse_result == AGENT_INVOKE_HTTP_INCOMPLETE && received > 0) {
        return;
    }
    if (parse_result != AGENT_INVOKE_HTTP_OK) {
        (void)agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_IO_FAILED);
        fail_gateway(state, 502,
                     parse_result == AGENT_INVOKE_HTTP_TOO_LARGE
                        ? "GATEWAY_RESPONSE_TOO_LARGE"
                        : "GATEWAY_PROTOCOL_ERROR");
        return;
    }
    if (!agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_RESPONSE_RECEIVED)) {
        fail_gateway(state, 502, "GATEWAY_PROTOCOL_ERROR");
        return;
    }
    send_mapped_gateway_response(
        state, response.status, response.content_type,
        state->backend_response + response.header_length,
        response.body_length);
}

static void deadline_callback(
    struct ev_loop *loop,
    ev_timer *watcher,
    int revents
)
{
    struct adapterd_request *state = watcher->data;

    (void)loop;
    (void)revents;
    runtime.gateway_timeouts++;
    (void)agent_invoke_backend_machine_transition(
        &state->backend_machine, AGENT_BACKEND_DEADLINE_EXPIRED);
    if (state->route.streaming && state->stream_head_sent) {
        fail_stream(state);
    } else {
        fail_gateway(state, 504, "GATEWAY_TIMEOUT");
    }
}

static bool start_gateway_request(
    struct adapterd_request *state,
    const struct agent_adapter_credentials *credentials,
    const char *envelope,
    size_t envelope_length
)
{
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
    const struct sockaddr *address;
    socklen_t address_length;
    size_t request_capacity;
    int flags;
    int result;
    ev_tstamp timeout;
    struct agent_invoke_endpoint gateway;
    struct ev_loop *loop = state->connection->get_loop(state->connection);

    if (envelope_length > SIZE_MAX - ADAPTERD_MAX_HTTP_HEADER -
            2U * AGENT_ADAPTER_MAX_CREDENTIAL_LEN) {
        return false;
    }
    request_capacity = envelope_length + ADAPTERD_MAX_HTTP_HEADER +
        2U * AGENT_ADAPTER_MAX_CREDENTIAL_LEN + 1U;
    gateway = config.gateway;
    if (state->route.streaming &&
        snprintf(gateway.path, sizeof(gateway.path), "%s",
                 "/agent/v1/invoke-stream") >= (int)sizeof(gateway.path)) {
        return false;
    }
    state->backend_request = malloc(request_capacity);
    state->backend_response_capacity = (size_t)config.max_response_bytes +
        AGENT_INVOKE_MAX_HEADER_BYTES + 1U;
    state->backend_response = malloc(state->backend_response_capacity);
    if (state->route.streaming) {
        state->stream_event_buffer = malloc(config.max_stream_event_bytes);
    }
    if (state->backend_request == NULL || state->backend_response == NULL ||
        (state->route.streaming && state->stream_event_buffer == NULL) ||
        !agent_adapter_build_gateway_request(
            &gateway, credentials, state->route.streaming,
            envelope, envelope_length,
            state->backend_request, request_capacity,
            &state->backend_request_length)) {
        return false;
    }
    state->backend_response[0] = '\0';
    if (config.gateway.family == AGENT_INVOKE_IPV6) {
        memset(&ipv6, 0, sizeof(ipv6));
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(config.gateway.port);
        if (inet_pton(AF_INET6, config.gateway.host, &ipv6.sin6_addr) != 1) {
            return false;
        }
        state->backend_fd = socket(AF_INET6, SOCK_STREAM, 0);
        address = (const struct sockaddr *)&ipv6;
        address_length = sizeof(ipv6);
    } else {
        memset(&ipv4, 0, sizeof(ipv4));
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(config.gateway.port);
        if (inet_pton(AF_INET, config.gateway.host, &ipv4.sin_addr) != 1) {
            return false;
        }
        state->backend_fd = socket(AF_INET, SOCK_STREAM, 0);
        address = (const struct sockaddr *)&ipv4;
        address_length = sizeof(ipv4);
    }
    if (state->backend_fd < 0) {
        return false;
    }
    flags = fcntl(state->backend_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(state->backend_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    state->backend_started = true;
    agent_invoke_backend_machine_init(&state->backend_machine);
    ev_io_init(&state->backend_watcher, backend_event_callback,
               state->backend_fd, EV_WRITE);
    state->backend_watcher.data = state;
    timeout = (ev_tstamp)(state->route.streaming
        ? config.stream_idle_timeout_ms : state->effective_gateway_timeout_ms) /
        1000.0;
    ev_timer_init(&state->deadline_watcher, deadline_callback, timeout, 0.0);
    state->deadline_watcher.data = state;
    state->connection->incref(state->connection);
    state->retained = true;
    ev_timer_start(loop, &state->deadline_watcher);
    result = connect(state->backend_fd, address, address_length);
    if (result == 0) {
        if (!agent_invoke_backend_machine_transition(
                &state->backend_machine, AGENT_BACKEND_CONNECTED) ||
            !try_send_backend(state)) {
            return false;
        }
        return true;
    }
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
    }
    ev_io_start(loop, &state->backend_watcher);
    return true;
}

static bool copy_header(
    struct uh_connection *connection,
    const char *name,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    struct uh_str value = connection->get_header(connection, name);

    *output_length = 0U;
    if (value.p == NULL) {
        output[0] = '\0';
        return true;
    }
    if (value.len == 0U || value.len >= output_capacity) {
        return false;
    }
    memcpy(output, value.p, value.len);
    output[value.len] = '\0';
    *output_length = value.len;
    return true;
}

static bool header_contains(
    struct uh_connection *connection,
    const char *name,
    const char *needle
)
{
    struct uh_str value = connection->get_header(connection, name);
    size_t needle_length = strlen(needle);
    size_t index;

    if (value.p == NULL || value.len < needle_length) return false;
    for (index = 0U; index + needle_length <= value.len; index++) {
        if (strncasecmp(value.p + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool attach_resume_cursor(
    struct uh_connection *connection,
    struct json_object *envelope
)
{
    struct uh_str value = connection->get_header(connection, "Last-Event-ID");
    char text[32];
    char *end = NULL;
    unsigned long long parsed;
    size_t index;

    if (value.p == NULL) return true;
    if (value.len == 0U || value.len >= sizeof(text)) return false;
    for (index = 0U; index < value.len; index++) {
        if (value.p[index] < '0' || value.p[index] > '9') return false;
    }
    memcpy(text, value.p, value.len);
    text[value.len] = '\0';
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed > (unsigned long long)INT64_MAX) return false;
    json_object_object_add(
        envelope, "resume_from_event_id",
        json_object_new_int64((int64_t)parsed));
    return true;
}

static void ingress_handler(struct uh_connection *connection, int event)
{
    struct adapterd_request *state;

    if (event == UH_EV_HEAD_COMPLETE) {
        struct uh_str path;

        release_request(connection);
        state = calloc(1U, sizeof(*state));
        if (state == NULL) {
            connection->send_error(connection, 500, "OUT_OF_MEMORY");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        state->connection = connection;
        state->backend_fd = -1;
        connection->userdata = state;
        path = connection->get_path(connection);
        if (!agent_adapter_ingress_parse_path(
                path.p, path.len, &state->route)) {
            reject_head(connection, 404, "ADAPTER_ROUTE_NOT_FOUND");
            return;
        }
        /* GET/DELETE are unsupported, including standard MCP resumption.
         * Check this before POST-only Accept/body validation. */
        if (strcmp(connection->get_method_str(connection), "POST") != 0) {
            reject_head(connection, 405, "METHOD_NOT_ALLOWED");
            return;
        }
        if (state->route.protocol == AGENT_ADAPTER_PROTOCOL_MCP) {
            struct uh_str format = connection->get_header(
                connection, "X-Nexus-Stream-Format");
            if (format.p != NULL) {
                if (format.len != 8U || memcmp(format.p, "envelope", 8U) != 0) {
                    reject_head(connection, 400, "INVALID_STREAM_FORMAT");
                    return;
                }
                state->mcp_envelope_stream = true;
            }
            if (!state->mcp_envelope_stream &&
                connection->get_header(connection, "Last-Event-ID").p != NULL) {
                reject_head(connection, 400, "MCP_RESUME_UNSUPPORTED");
                return;
            }
        }
        if (state->route.protocol == AGENT_ADAPTER_PROTOCOL_MCP &&
            header_contains(connection, "Accept", "text/event-stream")) {
            if (!header_contains(connection, "Accept", "application/json")) {
                reject_head(connection, 406, "MCP_ACCEPT_INVALID");
                return;
            }
            state->route.streaming = true;
        }
        if (state->route.streaming &&
            !header_contains(connection, "Accept", "text/event-stream")) {
            reject_head(connection, 406, "SSE_REQUIRED");
            return;
        }
        if (state->route.streaming && !config.stream_enabled) {
            reject_head(connection, 404, "STREAM_DISABLED");
            return;
        }
        if (strcmp(connection->get_method_str(connection), "POST") != 0) {
            reject_head(connection, 405, "METHOD_NOT_ALLOWED");
            return;
        }
        if (!content_type_allowed(connection, state->route.protocol)) {
            reject_head(connection, 415, "UNSUPPORTED_MEDIA_TYPE");
            return;
        }
        if (connection->get_content_length(connection) == 0U ||
            connection->get_content_length(connection) >
                config.max_request_bytes) {
            reject_head(connection, 413, "REQUEST_TOO_LARGE");
            return;
        }
        if (runtime.active_requests >= config.max_inflight) {
            reject_head(connection, 503, "CONCURRENCY_LIMITED");
            return;
        }
        state->counted = true;
        runtime.active_requests++;
        runtime.admitted_requests++;
        connection->check_expect_100_continue(connection);
        return;
    }
    if (event != UH_EV_COMPLETE) {
        return;
    }
    if (connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    if (connection->userdata == NULL) {
        send_plain_error(connection, 500, "REQUEST_STATE_LOST",
                         "adapter request state is unavailable");
        return;
    }
    state = connection->userdata;
    {
        struct uh_str body = connection->get_body(connection);
        struct uh_str deadline = connection->get_header(
            connection, "X-Nexus-Deadline");
        struct adapter_codec_options options;
        enum adapter_codec_result codec_result;
        const char *serialized;
        char authorization[AGENT_ADAPTER_MAX_CREDENTIAL_LEN + 1U];
        char transaction[AGENT_ADAPTER_MAX_CREDENTIAL_LEN + 1U];
        char public_ipv6[AGENT_PUBLIC_INGRESS_IPV6_MAX];
        char edge_token[AGENT_PUBLIC_INGRESS_TOKEN_MAX];
        char run_context_url[ADAPTERD_CONTEXT_URL_LEN];
        char run_context_token[ADAPTERD_CONTEXT_TOKEN_LEN];
        size_t authorization_length;
        size_t transaction_length;
        size_t public_ipv6_length;
        size_t edge_token_length;
        size_t run_context_url_length;
        size_t run_context_token_length;
        struct agent_adapter_credentials credentials;
        char deadline_text[128U];

        if (state->route.protocol == AGENT_ADAPTER_PROTOCOL_MCP) {
            struct json_object *control_response = NULL;
            enum adapter_mcp_control_result control_result =
                adapter_codec_mcp_control(
                    &registry, state->route.authority,
                    body.p, body.len, &control_response);

            if (control_result == ADAPTER_MCP_CONTROL_HANDLED ||
                control_result == ADAPTER_MCP_CONTROL_NOTIFICATION) {
                struct uh_str ingress = connection->get_header(
                    connection, AGENT_PUBLIC_INGRESS_DEST_HEADER);
                struct uh_str authorization = connection->get_header(
                    connection, "Authorization");
                static const char bearer[] = "Bearer ";

                /* Public MCP discovery remains credential-gated. Invocation
                 * JWT signature, lifetime, scope and exact target checks are
                 * performed by agent-gw for tools/call. */
                if (ingress.p != NULL &&
                    (authorization.p == NULL ||
                     authorization.len <= sizeof(bearer) - 1U ||
                     strncmp(authorization.p, bearer,
                             sizeof(bearer) - 1U) != 0)) {
                    json_object_put(control_response);
                    send_plain_error(connection, 401,
                                     "AUTHENTICATION_REQUIRED",
                                     "public MCP discovery requires a bearer token");
                    runtime.invalid_requests++;
                    destroy_request(state);
                    return;
                }
                if (control_result == ADAPTER_MCP_CONTROL_NOTIFICATION) {
                    send_empty(connection, 202);
                } else {
                    send_json(connection, 200,
                              "application/json; charset=utf-8",
                              control_response);
                    json_object_put(control_response);
                }
                runtime.completed_requests++;
                destroy_request(state);
                return;
            }
            json_object_put(control_response);
            if (control_result == ADAPTER_MCP_CONTROL_OUT_OF_MEMORY) {
                send_plain_error(connection, 500, "OUT_OF_MEMORY",
                                 "failed to build MCP control response");
                destroy_request(state);
                return;
            }
        }

        memset(&options, 0, sizeof(options));
        options.protocol = state->route.protocol;
        options.authority = state->route.authority;
        options.selector = state->route.selector_from_request
            ? NULL : state->route.selector;
        options.tenant = config.tenant;
        options.source_agent = config.source_agent;
        options.region = config.region;
        options.hop_limit = config.hop_limit;
        if (deadline.p != NULL) {
            if (deadline.len == 0U || deadline.len >= sizeof(deadline_text)) {
                send_plain_error(connection, 400, "INVALID_DEADLINE",
                                 "deadline header is invalid");
                runtime.invalid_requests++;
                destroy_request(state);
                return;
            }
            memcpy(deadline_text, deadline.p, deadline.len);
            deadline_text[deadline.len] = '\0';
            options.deadline = deadline_text;
        }
        if (!copy_header(connection, "X-Nexus-Run-Context-Url",
                         run_context_url, sizeof(run_context_url),
                         &run_context_url_length) ||
            !copy_header(connection, "X-Nexus-Run-Context-Token",
                         run_context_token, sizeof(run_context_token),
                         &run_context_token_length) ||
            ((run_context_url_length == 0U) !=
             (run_context_token_length == 0U))) {
            send_plain_error(connection, 400, "INVALID_CONTEXT_HEADER",
                             "run context header exceeds the configured bound");
            runtime.invalid_requests++;
            destroy_request(state);
            return;
        }
        if (run_context_url_length > 0U) {
            options.run_context_url = run_context_url;
            options.run_context_token = run_context_token;
        }
        codec_result = adapter_codec_normalize(
            &registry, &options, body.p, body.len, &state->normalized);
        if (codec_result != ADAPTER_CODEC_OK) {
            int status = codec_result == ADAPTER_CODEC_ROUTE_NOT_FOUND
                ? 404 : (codec_result == ADAPTER_CODEC_OUT_OF_MEMORY ? 500 : 400);
            const char *code = codec_result == ADAPTER_CODEC_ROUTE_NOT_FOUND
                ? "CAPABILITY_NOT_FOUND" :
                (codec_result == ADAPTER_CODEC_OUT_OF_MEMORY
                    ? "OUT_OF_MEMORY" : "INVALID_PROTOCOL_REQUEST");

            if (codec_result == ADAPTER_CODEC_ROUTE_NOT_FOUND) {
                runtime.mapping_misses++;
                send_protocol_failure(state, status, code);
                return;
            } else {
                runtime.invalid_requests++;
            }
            send_plain_error(connection, status, code,
                             "protocol request could not be normalized");
            destroy_request(state);
            return;
        }
        state->effective_gateway_timeout_ms = state->normalized.interactive
            ? config.interactive_timeout_ms : config.gateway_timeout_ms;
        if (state->route.protocol == AGENT_ADAPTER_PROTOCOL_MCP &&
            !state->mcp_envelope_stream) {
            unsigned char nonce[16];
            size_t count = 0U, index;
            static const char hex[] = "0123456789abcdef";
            /* Stateless MCP: an RPC ID is response correlation, not a global
             * idempotency key. Each POST receives a fresh internal task ID.
             * Explicit Nexus envelope mode retains its own replay contract. */
            while (count < sizeof(nonce)) {
                ssize_t got = getrandom(nonce + count, sizeof(nonce) - count, 0);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) {
                    fail_gateway(state, 500, "TASK_ID_UNAVAILABLE");
                    return;
                }
                count += (size_t)got;
            }
            memcpy(state->normalized.task_id, "mcp:", 4U);
            for (index = 0U; index < sizeof(nonce); index++) {
                state->normalized.task_id[4U + 2U * index] = hex[nonce[index] >> 4U];
                state->normalized.task_id[5U + 2U * index] = hex[nonce[index] & 15U];
            }
            state->normalized.task_id[36U] = '\0';
            json_object_object_add(state->normalized.envelope, "task_id",
                json_object_new_string(state->normalized.task_id));
        }
        if (state->route.streaming &&
            !attach_resume_cursor(connection, state->normalized.envelope)) {
            send_plain_error(connection, 400, "INVALID_LAST_EVENT_ID",
                             "Last-Event-ID must be a non-negative integer");
            runtime.invalid_requests++;
            destroy_request(state);
            return;
        }
        if (!copy_header(connection, "Authorization", authorization,
                         sizeof(authorization), &authorization_length) ||
            !copy_header(connection, "Txn-Token", transaction,
                         sizeof(transaction), &transaction_length) ||
            !copy_header(connection, AGENT_PUBLIC_INGRESS_DEST_HEADER,
                         public_ipv6, sizeof(public_ipv6),
                         &public_ipv6_length) ||
            !copy_header(connection, AGENT_PUBLIC_INGRESS_TOKEN_HEADER,
                         edge_token, sizeof(edge_token),
                         &edge_token_length) ||
            ((public_ipv6_length == 0U) != (edge_token_length == 0U))) {
            send_plain_error(connection, 400, "INVALID_CREDENTIAL_HEADER",
                             "credential header exceeds the configured bound");
            runtime.invalid_requests++;
            destroy_request(state);
            return;
        }
        memset(&credentials, 0, sizeof(credentials));
        if (authorization_length > 0U) {
            credentials.authorization = authorization;
            credentials.authorization_length = authorization_length;
        }
        if (transaction_length > 0U) {
            credentials.transaction_token = transaction;
            credentials.transaction_token_length = transaction_length;
        }
        if (public_ipv6_length > 0U) {
            credentials.public_ipv6 = public_ipv6;
            credentials.public_ipv6_length = public_ipv6_length;
            credentials.edge_token = edge_token;
            credentials.edge_token_length = edge_token_length;
        }
        size_t serialized_length = 0U;

        if (!adapter_codec_serialize_envelope(
                state->normalized.envelope, &serialized,
                &serialized_length)) {
            send_plain_error(connection, 400, "INVALID_UTF8_ENVELOPE",
                             "normalized Envelope is not valid UTF-8 JSON");
            runtime.invalid_requests++;
            destroy_request(state);
            return;
        }
        if (!start_gateway_request(state, &credentials,
                                   serialized, serialized_length)) {
            runtime.gateway_failures++;
            send_plain_error(connection, 502, "GATEWAY_UNAVAILABLE",
                             "failed to start gateway request");
            destroy_request(state);
        }
    }
}

static void health_handler(struct uh_connection *connection, int event)
{
    struct json_object *root;

    if (event != UH_EV_COMPLETE) {
        return;
    }
    if (strcmp(connection->get_method_str(connection), "GET") != 0) {
        send_plain_error(connection, 405, "METHOD_NOT_ALLOWED",
                         "use GET for this endpoint");
        return;
    }
    root = json_object_new_object();
    if (root == NULL) {
        send_plain_error(connection, 500, "OUT_OF_MEMORY",
                         "failed to build health response");
        return;
    }
    json_object_object_add(root, "status", json_object_new_string("ok"));
    json_object_object_add(root, "phase", json_object_new_string("P2.7.3"));
    json_object_object_add(root, "validated_mappings",
                           json_object_new_uint64(registry.count));
    json_object_object_add(root, "dynamic_manifest_generation",
                           json_object_new_uint64(
                               runtime.manifest_generation));
    json_object_object_add(root, "dynamic_mappings",
                           json_object_new_uint64(runtime.dynamic_mappings));
    json_object_object_add(root, "identical_mappings",
                           json_object_new_uint64(
                               runtime.identical_mappings));
    json_object_object_add(root, "mapping_conflicts",
                           json_object_new_uint64(runtime.mapping_conflicts));
    json_object_object_add(root, "manifest_refresh_failures",
                           json_object_new_uint64(
                               runtime.manifest_refresh_failures));
    json_object_object_add(root, "active_requests",
                           json_object_new_uint64(runtime.active_requests));
    json_object_object_add(root, "admitted_requests",
                           json_object_new_uint64(runtime.admitted_requests));
    json_object_object_add(root, "completed_requests",
                           json_object_new_uint64(runtime.completed_requests));
    json_object_object_add(root, "invalid_requests",
                           json_object_new_uint64(runtime.invalid_requests));
    json_object_object_add(root, "mapping_misses",
                           json_object_new_uint64(runtime.mapping_misses));
    json_object_object_add(root, "gateway_failures",
                           json_object_new_uint64(runtime.gateway_failures));
    json_object_object_add(root, "gateway_timeouts",
                           json_object_new_uint64(runtime.gateway_timeouts));
    json_object_object_add(root, "client_disconnects",
                           json_object_new_uint64(runtime.client_disconnects));
    json_object_object_add(root, "stream_enabled",
                           json_object_new_boolean(config.stream_enabled));
    json_object_object_add(root, "streams_started",
                           json_object_new_uint64(runtime.streams_started));
    json_object_object_add(root, "streams_completed",
                           json_object_new_uint64(runtime.streams_completed));
    json_object_object_add(root, "stream_failures",
                           json_object_new_uint64(runtime.stream_failures));
    json_object_object_add(root, "stream_events",
                           json_object_new_uint64(runtime.stream_events));
    json_object_object_add(root, "stream_bytes",
                           json_object_new_uint64(runtime.stream_bytes));
    send_json(connection, 200, "application/json; charset=utf-8", root);
    json_object_put(root);
}

static void not_found_handler(struct uh_connection *connection, int event)
{
    if (event == UH_EV_COMPLETE) {
        send_plain_error(connection, 404, "NOT_FOUND", "endpoint not found");
    }
}

static void signal_callback(
    struct ev_loop *loop,
    ev_signal *watcher,
    int revents
)
{
    (void)watcher;
    (void)revents;
    ev_break(loop, EVBREAK_ALL);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [-a loopback:port] [-g gateway-url] [-c uci-package] "
            "[-D uci-dir] [-b max-request] [-R max-response] "
            "[-t gateway-timeout-ms] [-I interactive-timeout-ms] "
            "[-C max-inflight] [-T tenant] "
            "[-u source-agent] [-H hop-limit] [-r region] "
            "[-x stream-enabled] [-L stream-idle-timeout-ms] "
            "[-E max-stream-event-bytes] [-M manifest-file]\n",
            program);
}

static bool read_options(int argc, char **argv)
{
    uint32_t value;
    int option;

    memset(&config, 0, sizeof(config));
    if (!copy_text(config.listen, sizeof(config.listen),
                   ADAPTERD_DEFAULT_LISTEN) ||
        !copy_text(config.gateway_url, sizeof(config.gateway_url),
                   ADAPTERD_DEFAULT_GATEWAY) ||
        !copy_text(config.uci_package, sizeof(config.uci_package),
                   ADAPTERD_DEFAULT_UCI_PACKAGE) ||
        !copy_text(config.manifest_file, sizeof(config.manifest_file),
                   ADAPTERD_DEFAULT_MANIFEST_FILE) ||
        !copy_text(config.tenant, sizeof(config.tenant), "local") ||
        !copy_text(config.source_agent, sizeof(config.source_agent),
                   "agent://local/adapterd") ||
        !copy_text(config.region, sizeof(config.region), "local")) {
        return false;
    }
    config.hop_limit = 8U;
    config.max_request_bytes = ADAPTERD_DEFAULT_MAX_REQUEST;
    config.max_response_bytes = ADAPTERD_DEFAULT_MAX_RESPONSE;
    config.gateway_timeout_ms = ADAPTERD_DEFAULT_TIMEOUT_MS;
    config.interactive_timeout_ms =
        ADAPTERD_DEFAULT_INTERACTIVE_TIMEOUT_MS;
    config.max_inflight = ADAPTERD_DEFAULT_MAX_INFLIGHT;
    config.stream_enabled = false;
    config.stream_idle_timeout_ms = ADAPTERD_DEFAULT_STREAM_IDLE_TIMEOUT_MS;
    config.max_stream_event_bytes = ADAPTERD_DEFAULT_MAX_STREAM_EVENT;
    while ((option = getopt(argc, argv,
                            "a:g:c:D:b:R:t:I:C:T:u:H:r:x:L:E:M:h")) != -1) {
        switch (option) {
        case 'a':
            if (!copy_text(config.listen, sizeof(config.listen), optarg))
                return false;
            break;
        case 'g':
            if (!copy_text(config.gateway_url,
                           sizeof(config.gateway_url), optarg)) return false;
            break;
        case 'c':
            if (!copy_text(config.uci_package,
                           sizeof(config.uci_package), optarg)) return false;
            break;
        case 'D':
            if (!copy_text(config.uci_dir,
                           sizeof(config.uci_dir), optarg)) return false;
            break;
        case 'b':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > ADAPTERD_MAX_REQUEST_LIMIT) return false;
            config.max_request_bytes = value;
            break;
        case 'R':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > ADAPTERD_MAX_RESPONSE_LIMIT) return false;
            config.max_response_bytes = value;
            break;
        case 't':
            if (!parse_u32(optarg, &value) || value < 10U || value > 30000U)
                return false;
            config.gateway_timeout_ms = value;
            break;
        case 'I':
            if (!parse_u32(optarg, &value) || value < 1000U ||
                value > 3600000U) return false;
            config.interactive_timeout_ms = value;
            break;
        case 'C':
            if (!parse_u32(optarg, &value) || value == 0U || value > 1024U)
                return false;
            config.max_inflight = value;
            break;
        case 'T':
            if (!copy_text(config.tenant, sizeof(config.tenant), optarg))
                return false;
            break;
        case 'u':
            if (!copy_text(config.source_agent,
                           sizeof(config.source_agent), optarg)) return false;
            break;
        case 'H':
            if (!parse_u32(optarg, &value) || value < 2U || value > 255U)
                return false;
            config.hop_limit = value;
            break;
        case 'r':
            if (!copy_text(config.region, sizeof(config.region), optarg))
                return false;
            break;
        case 'x':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.stream_enabled = value == 1U;
            break;
        case 'L':
            if (!parse_u32(optarg, &value) || value < 100U ||
                value > 300000U) return false;
            config.stream_idle_timeout_ms = value;
            break;
        case 'E':
            if (!parse_u32(optarg, &value) || value < 64U ||
                value > ADAPTERD_MAX_RESPONSE_LIMIT) return false;
            config.max_stream_event_bytes = value;
            break;
        case 'M':
            if (!copy_text(config.manifest_file,
                           sizeof(config.manifest_file), optarg) ||
                strcmp(config.manifest_file,
                       ADAPTERD_DEFAULT_MANIFEST_FILE) != 0) return false;
            break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default: return false;
        }
    }
    return optind == argc && listen_is_loopback(config.listen) &&
           (!config.stream_enabled ||
            config.max_stream_event_bytes <= config.max_response_bytes) &&
           agent_invoke_parse_loopback_endpoint(
               config.gateway_url, &config.gateway) &&
           strcmp(config.gateway.path, "/agent/v1/invoke") == 0;
}

int main(int argc, char **argv)
{
    struct adapter_registry_load_result load_result;
    struct ev_loop *loop = EV_DEFAULT;
    struct uh_server *server;
    ev_signal interrupt_watcher;
    ev_signal terminate_watcher;

    if (!read_options(argc, argv)) {
        usage(argv[0]);
        fprintf(stderr, "agent-adapterd: invalid configuration\n");
        return EXIT_FAILURE;
    }
    if (!adapter_registry_load(
            config.uci_package,
            config.uci_dir[0] != '\0' ? config.uci_dir : NULL,
            &static_registry, &load_result)) {
        fprintf(stderr, "agent-adapterd: registry validation failed: %s\n",
                load_result.error);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    memset(&runtime, 0, sizeof(runtime));
    registry = static_registry;
    (void)refresh_dynamic_registry();
    server = uh_server_new(loop);
    if (server == NULL || server->listen(server, config.listen, false) < 1) {
        fprintf(stderr, "agent-adapterd: failed to listen on %s\n",
                config.listen);
        if (server != NULL) {
            server->free(server);
            free(server);
        }
        return EXIT_FAILURE;
    }
    server->https_redirect(server, false);
    server->set_conn_closed_cb(server, connection_closed);
    server->set_default_handler(server, not_found_handler);
    if (server->add_path_handler(server, "^/mcp/", ingress_handler) != 0 ||
        server->add_path_handler(server, "^/a2a/", ingress_handler) != 0 ||
        server->add_path_handler(server, "^/healthz$", health_handler) != 0) {
        fprintf(stderr, "agent-adapterd: failed to register handlers\n");
        server->free(server);
        free(server);
        return EXIT_FAILURE;
    }
    ev_signal_init(&interrupt_watcher, signal_callback, SIGINT);
    ev_signal_start(loop, &interrupt_watcher);
    ev_signal_init(&terminate_watcher, signal_callback, SIGTERM);
    ev_signal_start(loop, &terminate_watcher);
    ev_timer_init(&manifest_refresh_timer, manifest_refresh_callback,
                  1.0, 1.0);
    ev_timer_start(loop, &manifest_refresh_timer);
    ev_run(loop, 0);
    ev_timer_stop(loop, &manifest_refresh_timer);
    server->free(server);
    free(server);
    ev_loop_destroy(loop);
    return EXIT_SUCCESS;
}

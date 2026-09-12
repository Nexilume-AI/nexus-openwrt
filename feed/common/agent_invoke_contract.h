#ifndef NEXUS_AGENT_INVOKE_CONTRACT_H
#define NEXUS_AGENT_INVOKE_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_INVOKE_HOST_LEN 128U
#define AGENT_INVOKE_PATH_LEN 512U
#define AGENT_INVOKE_CONTENT_TYPE_LEN 128U
#define AGENT_INVOKE_MAX_HEADER_BYTES 8192U
#define AGENT_INVOKE_REMOTE_MAP_LEN 256U
#define AGENT_INVOKE_DYNAMIC_MAP_CAPACITY 64U
#define AGENT_INVOKE_ROUTE_ID_LEN 65U
#define AGENT_INVOKE_CA_BUNDLE_ID_LEN 64U
#define AGENT_INVOKE_CERT_SHA256_LEN 65U
#define AGENT_INVOKE_INTERNAL_TOKEN_LEN 64U
#define AGENT_INVOKE_INTERNAL_GENERATION_LEN 32U
#define AGENT_INVOKE_INTERNAL_PATH "/agent/v1/internal-invoke"
#define AGENT_INVOKE_INTERNAL_TOKEN_HEADER "X-Nexus-Internal-Token"
#define AGENT_INVOKE_INTERNAL_ENDPOINT_HEADER "X-Nexus-Selected-Endpoint"
#define AGENT_INVOKE_INTERNAL_ORIGIN_HEADER "X-Nexus-Selected-Origin"
#define AGENT_INVOKE_INTERNAL_TARGET_HEADER "X-Nexus-Target-Agent"
#define AGENT_INVOKE_INTERNAL_TIMEOUT_HEADER "X-Nexus-Timeout-Ms"

enum agent_invoke_transport {
    AGENT_INVOKE_HTTP = 0,
    AGENT_INVOKE_HTTPS = 1
};

enum agent_invoke_address_family {
    AGENT_INVOKE_DNS = 0,
    AGENT_INVOKE_IPV4 = 4,
    AGENT_INVOKE_IPV6 = 6
};

struct agent_invoke_endpoint {
    enum agent_invoke_transport transport;
    enum agent_invoke_address_family family;
    char host[AGENT_INVOKE_HOST_LEN];
    uint16_t port;
    char path[AGENT_INVOKE_PATH_LEN];
};

struct agent_invoke_remote_map {
    char identity[AGENT_INVOKE_HOST_LEN];
    uint16_t port;
    enum agent_invoke_address_family family;
    char address[46U];
};

struct agent_invoke_dynamic_map {
    bool active;
    char route_id[AGENT_INVOKE_ROUTE_ID_LEN];
    struct agent_invoke_endpoint endpoint;
    struct agent_invoke_remote_map mapping;
    char ca_bundle_id[AGENT_INVOKE_CA_BUNDLE_ID_LEN];
    char certificate_sha256[AGENT_INVOKE_CERT_SHA256_LEN];
    uint64_t expires_at_ms;
};

struct agent_invoke_dynamic_map_table {
    struct agent_invoke_dynamic_map entries[AGENT_INVOKE_DYNAMIC_MAP_CAPACITY];
    size_t count;
    uint64_t accepted;
    uint64_t renewed;
    uint64_t removed;
    uint64_t expired;
    uint64_t rejected;
};

enum agent_invoke_http_result {
    AGENT_INVOKE_HTTP_OK = 0,
    AGENT_INVOKE_HTTP_INCOMPLETE,
    AGENT_INVOKE_HTTP_INVALID,
    AGENT_INVOKE_HTTP_TOO_LARGE,
    AGENT_INVOKE_HTTP_REDIRECT
};

struct agent_invoke_http_response {
    int status;
    size_t header_length;
    size_t body_length;
    char content_type[AGENT_INVOKE_CONTENT_TYPE_LEN];
};

enum agent_invoke_deadline_result {
    AGENT_INVOKE_DEADLINE_OK = 0,
    AGENT_INVOKE_DEADLINE_INVALID,
    AGENT_INVOKE_DEADLINE_EXPIRED
};

enum agent_invoke_internal_selection_result {
    AGENT_INVOKE_INTERNAL_SELECTION_OK = 0,
    AGENT_INVOKE_INTERNAL_SELECTION_INVALID,
    AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH,
    AGENT_INVOKE_INTERNAL_SELECTION_NOT_LEASED
};

enum agent_invoke_backend_phase {
    AGENT_BACKEND_CONNECTING = 0,
    AGENT_BACKEND_SENDING,
    AGENT_BACKEND_RECEIVING,
    AGENT_BACKEND_FINISHED
};

enum agent_invoke_backend_event {
    AGENT_BACKEND_CONNECTED = 0,
    AGENT_BACKEND_REQUEST_SENT,
    AGENT_BACKEND_RESPONSE_RECEIVED,
    AGENT_BACKEND_DEADLINE_EXPIRED,
    AGENT_BACKEND_CLIENT_DISCONNECTED,
    AGENT_BACKEND_IO_FAILED
};

enum agent_invoke_backend_outcome {
    AGENT_BACKEND_ACTIVE = 0,
    AGENT_BACKEND_SUCCEEDED,
    AGENT_BACKEND_TIMED_OUT,
    AGENT_BACKEND_CANCELLED,
    AGENT_BACKEND_FAILED
};

struct agent_invoke_backend_machine {
    enum agent_invoke_backend_phase phase;
    enum agent_invoke_backend_outcome outcome;
};

bool agent_invoke_parse_loopback_endpoint(
    const char *endpoint,
    struct agent_invoke_endpoint *parsed
);

/*
 * Parse an automatically registered plain-HTTP Agent Server endpoint.
 * Only numeric RFC1918 IPv4 and IPv6 ULA addresses are accepted; DNS names,
 * public addresses, link-local addresses and user-info are deliberately kept
 * on the explicit TLS/public ingress paths.
 */
bool agent_invoke_parse_lan_endpoint(
    const char *endpoint,
    struct agent_invoke_endpoint *parsed
);

bool agent_invoke_parse_remote_tls_endpoint(
    const char *endpoint,
    struct agent_invoke_endpoint *parsed
);

bool agent_invoke_parse_remote_map(
    const char *mapping,
    struct agent_invoke_remote_map *parsed
);

bool agent_invoke_remote_map_matches(
    const struct agent_invoke_endpoint *endpoint,
    const struct agent_invoke_remote_map *mapping
);

void agent_invoke_dynamic_map_table_init(
    struct agent_invoke_dynamic_map_table *table
);

bool agent_invoke_dynamic_map_upsert(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    const struct agent_invoke_endpoint *endpoint,
    const struct agent_invoke_remote_map *mapping,
    const char *ca_bundle_id,
    const char *certificate_sha256,
    uint64_t expires_at_ms,
    uint64_t now_ms
);

bool agent_invoke_dynamic_map_renew(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    uint64_t expires_at_ms,
    uint64_t now_ms
);

bool agent_invoke_dynamic_map_remove(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id
);

const struct agent_invoke_dynamic_map *agent_invoke_dynamic_map_find(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    const struct agent_invoke_endpoint *endpoint,
    uint64_t now_ms
);

bool agent_invoke_dynamic_map_matches(
    const struct agent_invoke_dynamic_map *entry,
    const char *route_id,
    const struct agent_invoke_endpoint *endpoint,
    uint64_t now_ms
);

size_t agent_invoke_dynamic_map_prune(
    struct agent_invoke_dynamic_map_table *table,
    uint64_t now_ms
);

bool agent_invoke_decrement_hop_limit(
    uint32_t incoming,
    uint8_t *forwarded
);

bool agent_invoke_retry_declaration_valid(
    bool idempotent,
    bool allow_retry,
    const char *idempotency_key
);

bool agent_invoke_effective_timeout_ms(
    uint64_t now_epoch_ms,
    uint32_t configured_timeout_ms,
    const char *deadline,
    uint32_t *effective_timeout_ms
);

enum agent_invoke_deadline_result agent_invoke_resolve_timeout_ms(
    uint64_t now_epoch_ms,
    uint32_t configured_timeout_ms,
    const char *deadline,
    uint32_t *effective_timeout_ms
);

bool agent_invoke_build_http_request(
    const struct agent_invoke_endpoint *endpoint,
    const char *route_id,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

bool agent_invoke_build_stream_http_request(
    const struct agent_invoke_endpoint *endpoint,
    const char *route_id,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

/*
 * Build the private agentd -> agent-gw request used only after agentd has
 * selected a local HTTPS AFIB route.  The Gateway must authenticate the
 * fixed-size token and re-check the exact lease-bound route/endpoint pair
 * before using its existing TLS backend executor.
 */
bool agent_invoke_build_internal_http_request(
    const struct agent_invoke_endpoint *gateway,
    const char *route_id,
    const char *selected_endpoint,
    const char *selected_origin,
    const char *target_agent,
    const char *token,
    uint32_t timeout_ms,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

bool agent_invoke_internal_token_is_valid(
    const char *token,
    size_t token_length
);

bool agent_invoke_internal_token_matches(
    const char expected[AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U],
    const char *presented,
    size_t presented_length
);

bool agent_invoke_internal_generation_is_valid(
    const char *generation
);

size_t agent_invoke_backend_response_limit(
    bool internal_invoke,
    size_t configured_limit
);

enum agent_invoke_internal_selection_result
agent_invoke_internal_selection_validate(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    const char *selected_endpoint,
    const char *selected_origin,
    const char *target_agent,
    const char *envelope_target_agent,
    uint64_t now_ms,
    struct agent_invoke_endpoint *parsed_endpoint,
    struct agent_invoke_dynamic_map *lease_snapshot
);

enum agent_invoke_http_result agent_invoke_parse_http_response(
    const char *buffer,
    size_t buffer_length,
    size_t max_body_length,
    struct agent_invoke_http_response *response
);

void agent_invoke_backend_machine_init(
    struct agent_invoke_backend_machine *machine
);

bool agent_invoke_backend_machine_transition(
    struct agent_invoke_backend_machine *machine,
    enum agent_invoke_backend_event event
);

#endif

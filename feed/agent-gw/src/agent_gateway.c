#include "agent_gateway_contract.h"
#include "agent_auth_policy.h"
#include "agent_edge_proxy.h"
#include "agent_forwarding_assertion.h"
#include "agent_invoke_contract.h"
#include "agent_ipc_protocol.h"
#include "agent_lan_session.h"
#include "agent_public_ingress.h"
#include "agent_jwt_verifier.h"
#include "agent_rate_limiter.h"
#include "agent_replay_cache.h"
#include "agent_sse_contract.h"
#include "agent_stream_resume_cache.h"
#include "agent_tls_client.h"
#include "agent_tenant_quota.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <limits.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <uhttpd/uhttpd.h>
#include <unistd.h>

#define GATEWAY_DEFAULT_LISTEN "127.0.0.1:7788"
#define GATEWAY_DEFAULT_IPC_SOCKET "/var/run/agentd/lookup.sock"
#define GATEWAY_DEFAULT_MAX_ENVELOPE 65536U
#define GATEWAY_DEFAULT_IPC_TIMEOUT_MS 200U
#define GATEWAY_DEFAULT_MAX_INFLIGHT 32U
#define GATEWAY_DEFAULT_RATE_PER_SECOND 50U
#define GATEWAY_DEFAULT_RATE_BURST 100U
#define GATEWAY_DEFAULT_BACKEND_TIMEOUT_MS 5000U
#define GATEWAY_DEFAULT_INTERACTIVE_BACKEND_TIMEOUT_MS 3600000U
#define GATEWAY_DEFAULT_MAX_BACKEND_RESPONSE 262144U
#define GATEWAY_DEFAULT_STREAM_IDLE_TIMEOUT_MS 15000U
#define GATEWAY_DEFAULT_MAX_STREAM_EVENT 65536U
#define GATEWAY_DEFAULT_MAX_ROUTE_ATTEMPTS 2U
#define GATEWAY_DEFAULT_RETRY_ATTEMPT_TIMEOUT_MS 1500U
#define GATEWAY_DEFAULT_RETRY_MIN_REMAINING_MS 100U
#define GATEWAY_DEFAULT_STREAM_RESUME_CAPACITY 512U
#define GATEWAY_DEFAULT_STREAM_RESUME_TTL_SECONDS 300U
#define GATEWAY_MAX_ENVELOPE_LIMIT 1048576U
#define GATEWAY_MAX_BACKEND_RESPONSE_LIMIT 1048576U
#define GATEWAY_BACKEND_REQUEST_HEADER_RESERVE 1024U
#define GATEWAY_SOCKET_PATH_LEN 108U
#define GATEWAY_TASK_ID_LEN 128U
#define GATEWAY_MAX_REMOTE_MAPS 16U
#define GATEWAY_TLS_IDENTITY_LEN 254U
#define GATEWAY_CA_BUNDLE_ID_LEN 64U
#define GATEWAY_LAN_LIMITER_CAPACITY 64U
#define GATEWAY_CLOUD_STATUS_FILE \
    "/var/run/nexus-agent-cloud/registrations.json"
#define GATEWAY_CLOUD_ENABLED_FILE \
    "/var/run/nexus-agent-cloud/enabled"
#define GATEWAY_CLOUD_ENROLLED_FILE \
    "/var/run/nexus-agent-cloud/enrolled"
#define GATEWAY_CLOUD_TENANT_FILE \
    "/var/run/nexus-agent-cloud/tenant-id"
#define GATEWAY_MANIFEST_SNAPSHOT_FILE \
    "/var/run/agent-manifests/manifests.json"
#define GATEWAY_CLOUD_STATUS_MAX 1048576U
#define GATEWAY_MANIFEST_SNAPSHOT_MAX 1048576U
#define GATEWAY_CLOUD_ORIGIN_LEN 512U
#define GATEWAY_CLOUD_CA_MAX 65536U
#define CONNECTION_REJECTED ((void *)(uintptr_t)1U)

static bool utf8_bytes_valid(const char *text, size_t length)
{
    size_t index = 0U;

    if (text == NULL) return false;
    while (index < length) {
        unsigned char first = (unsigned char)text[index++];
        unsigned char second;
        unsigned char third;
        unsigned char fourth;

        if (first <= 0x7fU) continue;
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index >= length) return false;
            second = (unsigned char)text[index++];
            if (second < 0x80U || second > 0xbfU) return false;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (length - index < 2U) return false;
            second = (unsigned char)text[index++];
            third = (unsigned char)text[index++];
            if (third < 0x80U || third > 0xbfU ||
                (first == 0xe0U && (second < 0xa0U || second > 0xbfU)) ||
                (first == 0xedU && (second < 0x80U || second > 0x9fU)) ||
                (first != 0xe0U && first != 0xedU &&
                 (second < 0x80U || second > 0xbfU))) return false;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (length - index < 3U) return false;
            second = (unsigned char)text[index++];
            third = (unsigned char)text[index++];
            fourth = (unsigned char)text[index++];
            if (third < 0x80U || third > 0xbfU ||
                fourth < 0x80U || fourth > 0xbfU ||
                (first == 0xf0U && (second < 0x90U || second > 0xbfU)) ||
                (first == 0xf4U && (second < 0x80U || second > 0x8fU)) ||
                (first != 0xf0U && first != 0xf4U &&
                 (second < 0x80U || second > 0xbfU))) return false;
            continue;
        }
        return false;
    }
    return true;
}

static void append_json_unicode_escape(
    char *target,
    size_t *offset,
    uint16_t value
)
{
    static const char hex[] = "0123456789abcdef";

    target[(*offset)++] = '\\';
    target[(*offset)++] = 'u';
    target[(*offset)++] = hex[(value >> 12U) & 0x0fU];
    target[(*offset)++] = hex[(value >> 8U) & 0x0fU];
    target[(*offset)++] = hex[(value >> 4U) & 0x0fU];
    target[(*offset)++] = hex[value & 0x0fU];
}

static bool json_extensions_absent(const char *text, size_t length)
{
    bool in_string = false;
    bool escaped = false;
    size_t index;

    for (index = 0U; index < length; index++) {
        unsigned char current = (unsigned char)text[index];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                in_string = false;
            }
            continue;
        }
        if (current == '"') {
            in_string = true;
            continue;
        }
        if (current == '/') return false;
        if (current == ',') {
            size_t next = index + 1U;

            while (next < length &&
                   (text[next] == ' ' || text[next] == '\t' ||
                    text[next] == '\r' || text[next] == '\n')) next++;
            if (next < length && (text[next] == '}' || text[next] == ']')) {
                return false;
            }
            continue;
        }
        if ((current >= 'A' && current <= 'Z') ||
            (current >= 'a' && current <= 'z')) {
            size_t remaining = length - index;
            size_t token_length = 0U;

            if ((current == 'e' || current == 'E') && index > 0U &&
                text[index - 1U] >= '0' && text[index - 1U] <= '9') {
                size_t next = index + 1U;

                if (next < length &&
                    (text[next] == '+' || text[next] == '-')) next++;
                if (next < length && text[next] >= '0' && text[next] <= '9') {
                    continue;
                }
            }
            if (remaining >= 4U && memcmp(text + index, "true", 4U) == 0) {
                token_length = 4U;
            } else if (remaining >= 5U &&
                       memcmp(text + index, "false", 5U) == 0) {
                token_length = 5U;
            } else if (remaining >= 4U &&
                       memcmp(text + index, "null", 4U) == 0) {
                token_length = 4U;
            } else {
                return false;
            }
            index += token_length - 1U;
        }
    }
    return !in_string && !escaped;
}

static char *strict_json_ascii_copy(
    const char *text,
    size_t length,
    size_t *escaped_length
)
{
    char *escaped;
    size_t input = 0U;
    size_t output = 0U;

    if (escaped_length == NULL || !utf8_bytes_valid(text, length) ||
        !json_extensions_absent(text, length) ||
        length > (SIZE_MAX - 1U) / 3U) return NULL;
    escaped = malloc(length * 3U + 1U);
    if (escaped == NULL) return NULL;
    while (input < length) {
        unsigned char first = (unsigned char)text[input++];
        unsigned char second;
        unsigned char third;
        unsigned char fourth;
        uint32_t codepoint;

        if (first <= 0x7fU) {
            escaped[output++] = (char)first;
            continue;
        }
        if (first <= 0xdfU) {
            second = (unsigned char)text[input++];
            codepoint = ((uint32_t)(first & 0x1fU) << 6U) |
                ((uint32_t)second & 0x3fU);
        } else if (first <= 0xefU) {
            second = (unsigned char)text[input++];
            third = (unsigned char)text[input++];
            codepoint = ((uint32_t)(first & 0x0fU) << 12U) |
                (((uint32_t)second & 0x3fU) << 6U) |
                ((uint32_t)third & 0x3fU);
        } else {
            uint16_t high;
            uint16_t low;

            second = (unsigned char)text[input++];
            third = (unsigned char)text[input++];
            fourth = (unsigned char)text[input++];
            codepoint = ((uint32_t)(first & 0x07U) << 18U) |
                (((uint32_t)second & 0x3fU) << 12U) |
                (((uint32_t)third & 0x3fU) << 6U) |
                ((uint32_t)fourth & 0x3fU);
            codepoint -= 0x10000U;
            high = (uint16_t)(0xd800U + (codepoint >> 10U));
            low = (uint16_t)(0xdc00U + (codepoint & 0x03ffU));
            append_json_unicode_escape(escaped, &output, high);
            append_json_unicode_escape(escaped, &output, low);
            continue;
        }
        append_json_unicode_escape(escaped, &output, (uint16_t)codepoint);
    }
    escaped[output] = '\0';
    *escaped_length = output;
    return escaped;
}

enum gateway_operation {
    GATEWAY_OPERATION_ROUTE = 0,
    GATEWAY_OPERATION_INVOKE,
    GATEWAY_OPERATION_STREAM,
    GATEWAY_OPERATION_INTERNAL_INVOKE,
    GATEWAY_OPERATION_REGISTER,
    GATEWAY_OPERATION_RENEW,
    GATEWAY_OPERATION_UNREGISTER
};

struct gateway_config {
    char listen[128];
    char ipc_socket[GATEWAY_SOCKET_PATH_LEN];
    uint32_t max_envelope_bytes;
    uint32_t ipc_timeout_ms;
    uint32_t max_inflight;
    uint32_t rate_per_second;
    uint32_t rate_burst;
    bool jwt_required;
    char jwt_public_key[PATH_MAX];
    char jwt_jwks_file[PATH_MAX];
    char jwt_key_id[AGENT_JWT_KID_LEN];
    char jwt_issuer[AGENT_AUTH_ISSUER_LEN];
    char jwt_audience[AGENT_AUTH_AUDIENCE_LEN];
    uint32_t jwt_clock_skew_seconds;
    uint32_t jwt_max_lifetime_seconds;
    uint32_t transaction_replay_capacity;
    bool forwarding_assertion_enabled;
    char forwarding_private_key[PATH_MAX];
    char forwarding_key_id[AGENT_FORWARDING_KID_LEN];
    char forwarding_issuer[AGENT_FORWARDING_ISSUER_LEN];
    uint32_t forwarding_ttl_seconds;
    bool invoke_enabled;
    bool registration_enabled;
    bool stream_enabled;
    uint32_t backend_timeout_ms;
    uint32_t interactive_backend_timeout_ms;
    uint32_t max_backend_response_bytes;
    uint32_t stream_idle_timeout_ms;
    uint32_t max_stream_event_bytes;
    bool stream_resume_enabled;
    uint32_t stream_resume_capacity;
    uint32_t stream_resume_ttl_seconds;
    bool lan_backend_enabled;
    bool remote_backend_enabled;
    char remote_backend_ca_file[PATH_MAX];
    char remote_backend_ca_bundle_id[GATEWAY_CA_BUNDLE_ID_LEN];
    struct agent_invoke_remote_map remote_maps[GATEWAY_MAX_REMOTE_MAPS];
    size_t remote_map_count;
    bool retry_enabled;
    uint8_t max_route_attempts;
    uint32_t retry_attempt_timeout_ms;
    uint32_t retry_min_remaining_ms;
    bool public_ingress_enabled;
    char public_ingress_token_file[PATH_MAX];
    char public_ingress_token[AGENT_PUBLIC_INGRESS_TOKEN_MAX];
    bool public_descriptor_enabled;
    char public_scheme[6];
    char public_tls_server_name[GATEWAY_TLS_IDENTITY_LEN];
    char public_ca_bundle_id[GATEWAY_CA_BUNDLE_ID_LEN];
    uint16_t public_ingress_port;
    bool internal_invoke_enabled;
    char internal_invoke_token_digest_file[PATH_MAX];
    char internal_invoke_token_digest[
        AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U];
    char internal_invoke_generation[
        AGENT_INVOKE_INTERNAL_GENERATION_LEN + 1U];
    bool lan_bootstrap_enabled;
    char lan_bridge_token_file[PATH_MAX];
    char lan_bridge_token[AGENT_LAN_SESSION_SECRET_LEN + 1U];
    char lan_session_key_file[PATH_MAX];
    char lan_session_key[AGENT_LAN_SESSION_SECRET_LEN + 1U];
    char cloud_public_origin[GATEWAY_CLOUD_ORIGIN_LEN];
    char cloud_trust_ca_file[PATH_MAX];
    char cloud_trust_ca_pem[GATEWAY_CLOUD_CA_MAX + 1U];
    char cloud_trust_ca_sha256[65U];
    bool cloud_trust_ready;
    bool cloud_trust_system;
};

struct gateway_request_state {
    enum gateway_operation operation;
    bool counted;
    bool tenant_quota_counted;
    bool connection_retained;
    bool tenant_overridden;
    bool invoke;
    bool stream;
    bool backend_started;
    bool stream_head_sent;
    bool stream_head_parsed;
    bool backend_tls;
    bool backend_tcp_connected;
    bool retry_requested;
    bool retry_eligible;
    bool relay_ipc;
    bool resume_requested;
    bool candidate_lookup;
    bool resume_route_available;
    bool public_ingress;
    bool registration_backend_tls;
    bool registration_backend_tls_endpoint_bound;
    bool internal_invoke;
    bool internal_dynamic_map_pinned;
    bool lan_session;
    bool lan_identity_mismatch;
    bool interactive_requested;
    int ipc_fd;
    int backend_fd;
    struct uh_connection *connection;
    ev_io ipc_watcher;
    ev_timer deadline_watcher;
    ev_io backend_watcher;
    ev_timer backend_deadline_watcher;
    struct gateway_ipc_machine ipc_machine;
    struct gateway_authenticated_identity identity;
    struct agent_ipc_lookup_request request;
    struct agent_ipc_lookup_response response;
    struct agent_ipc_candidates_response candidates;
    struct agent_ipc_invoke_response relay_response;
    struct agent_ipc_register_request registration;
    struct agent_ipc_renew_request renewal;
    struct agent_ipc_unregister_request removal;
    struct agent_ipc_lease_response lease_response;
    struct agent_invoke_endpoint registration_tls_endpoint;
    struct agent_invoke_remote_map registration_tls_map;
    struct agent_invoke_dynamic_map internal_dynamic_map;
    char registration_ca_bundle_id[GATEWAY_CA_BUNDLE_ID_LEN];
    char registration_certificate_sha256[AGENT_INVOKE_CERT_SHA256_LEN];
    char backend_certificate_sha256[AGENT_INVOKE_CERT_SHA256_LEN];
    bool ipc_application_error;
    uint32_t ipc_application_error_code;
    char ipc_application_error_message[AGENT_IPC_ERROR_LEN];
    struct agent_invoke_backend_machine backend_machine;
    struct agent_invoke_endpoint backend_endpoint;
    struct agent_tls_client_connection tls_connection;
    uint8_t frame[AGENT_IPC_MAX_FRAME_SIZE];
    size_t frame_length;
    char task_id[GATEWAY_TASK_ID_LEN];
    char source_agent[AGENT_IPC_AGENT_ID_LEN];
    char resume_route_id[AGENT_IPC_ROUTE_ID_LEN];
    char public_ipv6[AGENT_IPC_IPV6_LEN];
    char request_deadline[32U];
    uint64_t resume_from_event_id;
    uint8_t forwarded_hop_limit;
    char authenticated_scope[AGENT_AUTH_SCOPE_LEN];
    char authenticated_target_agent[AGENT_IPC_URI_LEN];
    char lan_source_address[AGENT_LAN_SESSION_SOURCE_LEN];
    char *forward_body;
    size_t forward_body_length;
    char *backend_request;
    size_t backend_request_length;
    size_t backend_request_sent;
    char *backend_response;
    size_t backend_response_length;
    size_t backend_response_capacity;
    char *stream_event_buffer;
    struct agent_sse_relay stream_relay;
    uint32_t effective_backend_timeout_ms;
    size_t effective_max_backend_response_bytes;
    uint64_t backend_deadline_ms;
    size_t candidate_index;
    uint64_t relay_stream_bytes;
};

struct gateway_lan_source_limiter {
    char source_address[AGENT_LAN_SESSION_SOURCE_LEN];
    struct agent_rate_limiter limiter;
    uint64_t last_seen_ms;
    bool bootstrap_issued;
};

struct gateway_runtime {
    uint32_t active_requests;
    uint64_t admitted_requests;
    uint64_t concurrency_rejections;
    uint64_t rate_rejections;
    uint64_t tenant_concurrency_rejections;
    uint64_t tenant_rate_rejections;
    uint64_t tenant_quota_table_rejections;
    uint64_t ipc_completed;
    uint64_t ipc_failures;
    uint64_t ipc_timeouts;
    uint64_t client_disconnects;
    struct agent_rate_limiter rate_limiter;
    struct agent_tenant_quota_table tenant_quotas;
    uint64_t authentication_successes;
    uint64_t authentication_failures;
    bool last_auth_failure_recorded;
    char last_auth_failure_stage[32];
    enum agent_jwt_result last_auth_jwt_result;
    enum agent_auth_result last_auth_claim_result;
    uint64_t transaction_tokens_accepted;
    uint64_t transaction_replays_rejected;
    uint64_t transaction_cache_rejections;
    uint64_t forwarding_assertions_issued;
    uint64_t forwarding_assertion_failures;
    uint64_t invoke_completed;
    uint64_t invoke_failures;
    uint64_t backend_timeouts;
    uint64_t backend_endpoint_rejections;
    uint64_t streams_started;
    uint64_t streams_completed;
    uint64_t stream_failures;
    uint64_t stream_events;
    uint64_t stream_bytes;
    uint64_t relay_streams_started;
    uint64_t relay_streams_completed;
    uint64_t relay_stream_frames;
    uint64_t resume_routes_stored;
    uint64_t resume_requests;
    uint64_t resume_misses;
    uint64_t resume_route_unavailable;
    uint64_t resume_capacity_rejections;
    uint64_t remote_tls_handshakes;
    uint64_t remote_tls_failures;
    uint64_t retry_requests;
    uint64_t retry_attempts;
    uint64_t retry_successes;
    uint64_t retry_exhausted;
    uint64_t retry_budget_rejections;
    uint64_t public_ingress_requests;
    uint64_t public_ingress_rejections;
    uint64_t public_ingress_route_misses;
    char last_public_ipv6[AGENT_IPC_IPV6_LEN];
    uint64_t internal_invoke_requests;
    uint64_t internal_invoke_auth_rejections;
    uint64_t internal_invoke_route_rejections;
    uint64_t internal_invoke_completed;
    uint64_t internal_invoke_failures;
    uint64_t lan_bootstrap_issued;
    uint64_t lan_bootstrap_rejected;
    uint64_t lan_bootstrap_refreshed;
    uint64_t lan_session_authenticated;
    uint64_t lan_session_rejected;
    uint64_t cloud_tenant_aliases;
    uint64_t cloud_tenant_alias_rejections;
    struct gateway_lan_source_limiter
        lan_source_limiters[GATEWAY_LAN_LIMITER_CAPACITY];
};

static struct gateway_config config;
static struct gateway_runtime runtime;
static uint32_t next_request_id = 1U;

static struct agent_jwt_verifier jwt_verifier;
static struct agent_forwarding_signer forwarding_signer;
static struct agent_replay_cache replay_cache;
static struct agent_stream_resume_cache stream_resume_cache;
static struct agent_tls_client_global tls_client;
static struct agent_invoke_dynamic_map_table dynamic_backend_maps;
static void close_backend_attempt(struct gateway_request_state *state);
static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL ||
        value == NULL ||
        text[0] < '0' ||
        text[0] > '9') {
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

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return ((uint64_t)now.tv_sec * 1000U) +
           ((uint64_t)now.tv_nsec / 1000000U);
}
static uint64_t wall_clock_seconds(void)
{
    time_t now = time(NULL);

    if (now < 0) {
        return 0U;
    }
    return (uint64_t)now;
}


static bool listen_is_loopback(const char *listen_address)
{
    struct in_addr ipv4;
    struct in6_addr ipv6;
    const char *host_start;
    const char *host_end;
    const char *port;
    char host[INET6_ADDRSTRLEN];
    uint32_t port_number;
    size_t host_length;

    if (listen_address == NULL) {
        return false;
    }
    if (listen_address[0] == '[') {
        host_start = listen_address + 1;
        host_end = strchr(host_start, ']');
        if (host_end == NULL || host_end[1] != ':') {
            return false;
        }
        port = host_end + 2;
    } else {
        host_start = listen_address;
        host_end = strrchr(host_start, ':');
        if (host_end == NULL ||
            memchr(host_start, ':', (size_t)(host_end - host_start)) != NULL) {
            return false;
        }
        port = host_end + 1;
    }

    host_length = (size_t)(host_end - host_start);
    if (host_length == 0U || host_length >= sizeof(host) ||
        !parse_u32(port, &port_number) ||
        port_number == 0U ||
        port_number > 65535U) {
        return false;
    }
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';

    if (listen_address[0] == '[') {
        return inet_pton(AF_INET6, host, &ipv6) == 1 &&
               IN6_IS_ADDR_LOOPBACK(&ipv6);
    }
    return inet_pton(AF_INET, host, &ipv4) == 1 &&
           (ntohl(ipv4.s_addr) >> 24U) == 127U;
}

static bool ipc_path_is_safe(const char *socket_path)
{
    static const char prefix[] = "/var/run/agentd/";

    return socket_path != NULL &&
           strncmp(socket_path, prefix, sizeof(prefix) - 1U) == 0 &&
           strstr(socket_path, "..") == NULL &&
           strlen(socket_path) < GATEWAY_SOCKET_PATH_LEN;
}

static bool internal_token_digest_path_is_safe(const char *digest_path)
{
    return digest_path != NULL &&
           strcmp(digest_path,
                  "/etc/agent-gw/nexus-cloud-gateway-token.sha256") == 0;
}

static bool read_internal_invoke_token_digest(void)
{
    struct stat metadata;
    size_t used = 0U;
    ssize_t result;
    char extra;
    int descriptor;

    config.internal_invoke_token_digest[0] = '\0';
    if (!config.internal_invoke_enabled) return true;
    descriptor = open(config.internal_invoke_token_digest_file,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0777U) != 0640U) {
        if (descriptor >= 0) close(descriptor);
        return false;
    }
    while (used < AGENT_INVOKE_INTERNAL_TOKEN_LEN) {
        result = read(descriptor, config.internal_invoke_token_digest + used,
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
        !agent_invoke_internal_token_is_valid(
            config.internal_invoke_token_digest, used)) {
        memset(config.internal_invoke_token_digest, 0,
               sizeof(config.internal_invoke_token_digest));
        return false;
    }
    config.internal_invoke_token_digest[used] = '\0';
    return true;
}

static bool internal_invoke_token_matches_digest(
    const char *presented,
    size_t presented_length
)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    char encoded[AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U];
    size_t index;
    bool matches;

    if (!agent_invoke_internal_token_is_valid(
            presented, presented_length) ||
        config.internal_invoke_token_digest[0] == '\0' ||
        mbedtls_sha256(
            (const unsigned char *)presented, presented_length,
            digest, 0) != 0) {
        return false;
    }
    for (index = 0U; index < sizeof(digest); index++) {
        encoded[index * 2U] = hex[digest[index] >> 4U];
        encoded[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    encoded[AGENT_INVOKE_INTERNAL_TOKEN_LEN] = '\0';
    matches = agent_invoke_internal_token_matches(
        config.internal_invoke_token_digest, encoded,
        AGENT_INVOKE_INTERNAL_TOKEN_LEN);
    memset(digest, 0, sizeof(digest));
    memset(encoded, 0, sizeof(encoded));
    return matches;
}

static bool jwt_key_path_is_safe(const char *key_path)
{
    static const char prefix[] = "/etc/agent-gw/";

    return key_path != NULL &&
           strncmp(key_path, prefix, sizeof(prefix) - 1U) == 0 &&
           strstr(key_path, "..") == NULL &&
           strlen(key_path) < PATH_MAX;
}

static bool backend_ca_path_is_safe(const char *ca_path)
{
    return ca_path != NULL &&
           (strcmp(ca_path, "/etc/ssl/certs/ca-certificates.crt") == 0 ||
            jwt_key_path_is_safe(ca_path));
}

static bool cloud_origin_is_safe(const char *origin)
{
    const char *authority;
    const char *cursor;
    size_t length;

    if (origin == NULL || strncmp(origin, "https://", 8U) != 0) return false;
    length = strlen(origin);
    if (length <= 8U || length >= GATEWAY_CLOUD_ORIGIN_LEN ||
        origin[length - 1U] == '/' || strchr(origin, '@') != NULL ||
        strchr(origin, '?') != NULL || strchr(origin, '#') != NULL) return false;
    authority = origin + 8U;
    for (cursor = authority; *cursor != '\0'; cursor++) {
        unsigned char character = (unsigned char)*cursor;
        if (character <= 0x20U || character == 0x7fU || character == '/') {
            return false;
        }
    }
    return authority[0] != '\0';
}

static bool cloud_ca_pem_is_certificate_only(const char *pem)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    const char *cursor = pem;
    size_t certificates = 0U;

    if (pem == NULL) return false;
    while (*cursor != '\0') {
        const char *closing;
        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n') cursor++;
        if (*cursor == '\0') break;
        if (strncmp(cursor, begin, sizeof(begin) - 1U) != 0) return false;
        closing = strstr(cursor + sizeof(begin) - 1U, end);
        if (closing == NULL) return false;
        cursor = closing + sizeof(end) - 1U;
        certificates++;
    }
    return certificates > 0U;
}

static bool read_cloud_trust(void)
{
    static const char system_ca[] = "/etc/ssl/certs/ca-certificates.crt";
    static const char hex[] = "0123456789abcdef";
    struct stat metadata;
    mbedtls_x509_crt certificates;
    unsigned char digest[32U];
    size_t used = 0U;
    size_t index;
    int descriptor = -1;
    bool parsed = false;

    config.cloud_trust_ready = false;
    config.cloud_trust_system = false;
    config.cloud_trust_ca_pem[0] = '\0';
    config.cloud_trust_ca_sha256[0] = '\0';
    if (config.cloud_public_origin[0] == '\0') return true;
    if (!cloud_origin_is_safe(config.cloud_public_origin) ||
        !backend_ca_path_is_safe(config.cloud_trust_ca_file)) return true;
    if (strcmp(config.cloud_trust_ca_file, system_ca) == 0) {
        config.cloud_trust_system = true;
        config.cloud_trust_ready = true;
        return true;
    }
    descriptor = open(config.cloud_trust_ca_file,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0022U) != 0U || metadata.st_size <= 0 ||
        (uint64_t)metadata.st_size > GATEWAY_CLOUD_CA_MAX) goto done;
    while (used < (size_t)metadata.st_size) {
        ssize_t count = read(descriptor, config.cloud_trust_ca_pem + used,
                             (size_t)metadata.st_size - used);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        goto done;
    }
    config.cloud_trust_ca_pem[used] = '\0';
    if (strlen(config.cloud_trust_ca_pem) != used ||
        !cloud_ca_pem_is_certificate_only(config.cloud_trust_ca_pem)) {
        goto done;
    }
    mbedtls_x509_crt_init(&certificates);
    if (mbedtls_x509_crt_parse(
            &certificates,
            (const unsigned char *)config.cloud_trust_ca_pem,
            used + 1U) == 0) parsed = true;
    mbedtls_x509_crt_free(&certificates);
    if (!parsed || mbedtls_sha256(
            (const unsigned char *)config.cloud_trust_ca_pem,
            used, digest, 0) != 0) goto done;
    for (index = 0U; index < sizeof(digest); index++) {
        config.cloud_trust_ca_sha256[index * 2U] = hex[digest[index] >> 4U];
        config.cloud_trust_ca_sha256[index * 2U + 1U] =
            hex[digest[index] & 0x0fU];
    }
    config.cloud_trust_ca_sha256[64U] = '\0';
    config.cloud_trust_ready = true;

done:
    memset(digest, 0, sizeof(digest));
    if (descriptor >= 0) close(descriptor);
    if (!config.cloud_trust_ready) {
        memset(config.cloud_trust_ca_pem, 0,
               sizeof(config.cloud_trust_ca_pem));
        memset(config.cloud_trust_ca_sha256, 0,
               sizeof(config.cloud_trust_ca_sha256));
    }
    /* Invalid optional Cloud trust must not disable LAN registration. */
    return true;
}

static bool descriptor_name_is_safe(const char *text, bool allow_underscore)
{
    size_t index;
    size_t length;

    if (text == NULL || text[0] == '\0') return false;
    length = strlen(text);
    if (text[0] == '.' || text[length - 1U] == '.') return false;
    for (index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)text[index];
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '.' ||
            (allow_underscore && character == '_')) {
            continue;
        }
        return false;
    }
    return true;
}

static bool read_public_ingress_token(void)
{
    FILE *file;
    size_t length;
    size_t index;
    int extra;

    if (!config.public_ingress_enabled) return true;
    file = fopen(config.public_ingress_token_file, "rb");
    if (file == NULL) return false;
    length = fread(config.public_ingress_token, 1U,
                   sizeof(config.public_ingress_token) - 1U, file);
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    extra = fgetc(file);
    if (extra != EOF || fclose(file) != 0) return false;
    while (length > 0U &&
           (config.public_ingress_token[length - 1U] == '\n' ||
            config.public_ingress_token[length - 1U] == '\r')) length--;
    if (length < 32U) return false;
    config.public_ingress_token[length] = '\0';
    for (index = 0U; index < length; index++) {
        unsigned char character =
            (unsigned char)config.public_ingress_token[index];
        if (character < 0x21U || character > 0x7eU) return false;
    }
    return true;
}

static bool read_lan_secret(
    const char *path,
    char output[AGENT_LAN_SESSION_SECRET_LEN + 1U]
)
{
    struct stat metadata;
    size_t used = 0U;
    ssize_t result;
    char extra;
    int descriptor;

    if (path == NULL || output == NULL ||
        strncmp(path, "/var/run/agent-gw/", 18U) != 0 ||
        strstr(path, "..") != NULL) return false;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) ||
        ((metadata.st_mode & 0777U) != 0600U &&
         (metadata.st_mode & 0777U) != 0640U)) {
        if (descriptor >= 0) close(descriptor);
        return false;
    }
    while (used < AGENT_LAN_SESSION_SECRET_LEN) {
        result = read(descriptor, output + used,
                      AGENT_LAN_SESSION_SECRET_LEN - used);
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
    if (result != 0 || close(descriptor) != 0) {
        memset(output, 0, AGENT_LAN_SESSION_SECRET_LEN + 1U);
        return false;
    }
    output[AGENT_LAN_SESSION_SECRET_LEN] = '\0';
    if (!agent_lan_session_secret_is_valid(output)) {
        memset(output, 0, AGENT_LAN_SESSION_SECRET_LEN + 1U);
        return false;
    }
    return true;
}

static struct json_object *read_bounded_json_file(
    const char *path,
    size_t maximum
)
{
    struct stat metadata;
    struct json_tokener *tokener = NULL;
    struct json_object *root = NULL;
    char *body = NULL;
    size_t used = 0U;
    int descriptor = -1;

    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0022U) != 0U || metadata.st_size <= 0 ||
        (uint64_t)metadata.st_size > maximum) goto failed;
    body = malloc((size_t)metadata.st_size + 1U);
    if (body == NULL) goto failed;
    while (used < (size_t)metadata.st_size) {
        ssize_t count = read(descriptor, body + used,
                             (size_t)metadata.st_size - used);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        goto failed;
    }
    body[used] = '\0';
    tokener = json_tokener_new_ex(32);
    if (tokener == NULL) goto failed;
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tokener, body, (int)used);
    if (root == NULL || json_tokener_get_error(tokener) != json_tokener_success ||
        json_tokener_get_parse_end(tokener) != used ||
        !json_object_is_type(root, json_type_object)) {
        if (root != NULL) json_object_put(root);
        root = NULL;
    }

failed:
    if (descriptor >= 0) close(descriptor);
    if (tokener != NULL) json_tokener_free(tokener);
    free(body);
    return root;
}

static bool read_cloud_transport_mode(char output[16U])
{
    struct stat metadata;
    ssize_t count;
    size_t length;
    int descriptor = open(GATEWAY_CLOUD_ENABLED_FILE,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0022U) != 0U || metadata.st_size <= 0 ||
        metadata.st_size >= 16) goto failed;
    do {
        count = read(descriptor, output, (size_t)metadata.st_size);
    } while (count < 0 && errno == EINTR);
    if (count != metadata.st_size || close(descriptor) != 0) {
        descriptor = -1;
        goto failed;
    }
    descriptor = -1;
    length = (size_t)count;
    while (length > 0U && (output[length - 1U] == '\n' ||
                           output[length - 1U] == '\r')) length--;
    output[length] = '\0';
    return strcmp(output, "direct_ipv6") == 0 ||
           strcmp(output, "relay") == 0 || strcmp(output, "auto") == 0;

failed:
    if (descriptor >= 0) close(descriptor);
    output[0] = '\0';
    return false;
}

static bool cloud_tenant_id_valid(const char *value)
{
    size_t index;

    if (value == NULL || strlen(value) != 36U) return false;
    for (index = 0U; index < 36U; index++) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool read_cloud_tenant_id(
    char output[AGENT_IPC_TENANT_LEN]
)
{
    struct stat metadata;
    ssize_t count;
    size_t length;
    int descriptor = open(GATEWAY_CLOUD_TENANT_FILE,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0022U) != 0U || metadata.st_size <= 0 ||
        (uint64_t)metadata.st_size >= AGENT_IPC_TENANT_LEN) goto failed;
    do {
        count = read(descriptor, output, (size_t)metadata.st_size);
    } while (count < 0 && errno == EINTR);
    if (count != metadata.st_size || close(descriptor) != 0) {
        descriptor = -1;
        goto failed;
    }
    descriptor = -1;
    length = (size_t)count;
    while (length > 0U && (output[length - 1U] == '\n' ||
                           output[length - 1U] == '\r')) length--;
    output[length] = '\0';
    return cloud_tenant_id_valid(output);

failed:
    if (descriptor >= 0) close(descriptor);
    output[0] = '\0';
    return false;
}

static bool apply_cloud_tenant_alias(struct gateway_request_state *state)
{
    struct json_object *root = NULL;
    struct json_object *manifests;
    char cloud_tenant[AGENT_IPC_TENANT_LEN] = {0};
    char local_tenant[AGENT_IPC_TENANT_LEN] = {0};
    size_t index;
    bool matched = false;

    if (state == NULL || state->lan_session || !state->identity.verified ||
        state->authenticated_target_agent[0] == '\0' ||
        state->request.target_agent[0] == '\0' ||
        strcmp(state->authenticated_target_agent,
               state->request.target_agent) != 0 ||
        !read_cloud_tenant_id(cloud_tenant) ||
        strcmp(state->request.tenant, cloud_tenant) != 0) return false;
    root = read_bounded_json_file(
        GATEWAY_MANIFEST_SNAPSHOT_FILE, GATEWAY_MANIFEST_SNAPSHOT_MAX);
    if (root == NULL ||
        !json_object_object_get_ex(root, "manifests", &manifests) ||
        !json_object_is_type(manifests, json_type_array)) goto done;
    for (index = 0U; index < json_object_array_length(manifests); index++) {
        struct json_object *item =
            json_object_array_get_idx(manifests, index);
        struct json_object *publish;
        struct json_object *origin;
        struct json_object *tenant;
        struct json_object *tool;
        struct json_object *authority;
        struct json_object *intent;
        const char *tenant_text;

        if (item == NULL || !json_object_is_type(item, json_type_object) ||
            !json_object_object_get_ex(item, "publish", &publish) ||
            !json_object_is_type(publish, json_type_boolean) ||
            !json_object_get_boolean(publish) ||
            !json_object_object_get_ex(item, "origin", &origin) ||
            !json_object_is_type(origin, json_type_string) ||
            strcmp(json_object_get_string(origin),
                   state->request.target_agent) != 0 ||
            !json_object_object_get_ex(item, "tenant", &tenant) ||
            !json_object_is_type(tenant, json_type_string) ||
            !json_object_object_get_ex(item, "tool", &tool) ||
            !json_object_is_type(tool, json_type_object) ||
            !json_object_object_get_ex(tool, "authority", &authority) ||
            !json_object_is_type(authority, json_type_string) ||
            strcmp(json_object_get_string(authority),
                   state->request.target_agent) != 0 ||
            !json_object_object_get_ex(tool, "intent", &intent) ||
            !json_object_is_type(intent, json_type_string) ||
            strcmp(json_object_get_string(intent), state->request.intent) != 0)
            continue;
        tenant_text = json_object_get_string(tenant);
        if (tenant_text == NULL || tenant_text[0] == '\0' ||
            strlen(tenant_text) >= sizeof(local_tenant)) goto done;
        if (matched && strcmp(local_tenant, tenant_text) != 0) goto done;
        if (!matched && !copy_text(local_tenant, sizeof(local_tenant),
                                   tenant_text)) goto done;
        matched = true;
    }
    if (matched && copy_text(state->request.tenant,
                             sizeof(state->request.tenant), local_tenant)) {
        runtime.cloud_tenant_aliases++;
        json_object_put(root);
        return true;
    }

done:
    runtime.cloud_tenant_alias_rejections++;
    if (root != NULL) json_object_put(root);
    return false;
}

static bool manifest_allows_interactive_timeout(
    const struct gateway_request_state *state
)
{
    struct json_object *root = NULL;
    struct json_object *manifests;
    size_t index;
    bool allowed = false;

    if (state == NULL || !state->interactive_requested ||
        state->request.target_agent[0] == '\0' ||
        state->request.intent[0] == '\0') return false;
    root = read_bounded_json_file(
        GATEWAY_MANIFEST_SNAPSHOT_FILE, GATEWAY_MANIFEST_SNAPSHOT_MAX);
    if (root == NULL ||
        !json_object_object_get_ex(root, "manifests", &manifests) ||
        !json_object_is_type(manifests, json_type_array)) goto done;
    for (index = 0U; index < json_object_array_length(manifests); index++) {
        struct json_object *item =
            json_object_array_get_idx(manifests, index);
        struct json_object *publish;
        struct json_object *origin;
        struct json_object *tool;
        struct json_object *intent;
        struct json_object *task = NULL;
        struct json_object *chat = NULL;
        struct json_object *interactive = NULL;

        if (item == NULL || !json_object_is_type(item, json_type_object) ||
            !json_object_object_get_ex(item, "publish", &publish) ||
            !json_object_is_type(publish, json_type_boolean) ||
            !json_object_get_boolean(publish) ||
            !json_object_object_get_ex(item, "origin", &origin) ||
            !json_object_is_type(origin, json_type_string) ||
            strcmp(json_object_get_string(origin),
                   state->request.target_agent) != 0 ||
            !json_object_object_get_ex(item, "tool", &tool) ||
            !json_object_is_type(tool, json_type_object) ||
            !json_object_object_get_ex(tool, "intent", &intent) ||
            !json_object_is_type(intent, json_type_string) ||
            strcmp(json_object_get_string(intent), state->request.intent) != 0)
            continue;
        (void)json_object_object_get_ex(tool, "task", &task);
        (void)json_object_object_get_ex(tool, "chat", &chat);
        (void)json_object_object_get_ex(tool, "interactive", &interactive);
        allowed =
            (task != NULL && json_object_is_type(task, json_type_boolean) &&
             json_object_get_boolean(task)) ||
            (chat != NULL && json_object_is_type(chat, json_type_boolean) &&
             json_object_get_boolean(chat)) ||
            (interactive != NULL &&
             json_object_is_type(interactive, json_type_boolean) &&
             json_object_get_boolean(interactive));
        if (allowed) break;
    }

done:
    if (root != NULL) json_object_put(root);
    return allowed;
}

static void apply_interactive_backend_timeout(
    struct gateway_request_state *state
)
{
    uint32_t effective;

    if (!manifest_allows_interactive_timeout(state)) return;
    if (agent_invoke_resolve_timeout_ms(
            wall_clock_seconds() * 1000U,
            config.interactive_backend_timeout_ms,
            state->request_deadline[0] != '\0'
                ? state->request_deadline : NULL,
            &effective) == AGENT_INVOKE_DEADLINE_OK) {
        state->effective_backend_timeout_ms = effective;
    }
}

static bool source_address_is_trusted_lan(const char *source)
{
    struct in_addr ipv4;
    struct in6_addr ipv6;
    uint32_t address;

    if (source == NULL || source[0] == '\0') return false;
    if (inet_pton(AF_INET, source, &ipv4) == 1) {
        address = ntohl(ipv4.s_addr);
        return (address & 0xff000000U) == 0x0a000000U ||
               (address & 0xfff00000U) == 0xac100000U ||
               (address & 0xffff0000U) == 0xc0a80000U;
    }
    if (inet_pton(AF_INET6, source, &ipv6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&ipv6)) {
            memcpy(&address, &ipv6.s6_addr[12], sizeof(address));
            address = ntohl(address);
            return (address & 0xff000000U) == 0x0a000000U ||
                   (address & 0xfff00000U) == 0xac100000U ||
                   (address & 0xffff0000U) == 0xc0a80000U;
        }
        return (ipv6.s6_addr[0] & 0xfeU) == 0xfcU;
    }
    return false;
}

static bool capture_trusted_lan_source(
    struct uh_connection *connection,
    char output[AGENT_LAN_SESSION_SOURCE_LEN]
)
{
    struct uh_str source = connection->get_header(
        connection, AGENT_LAN_SOURCE_HEADER);
    struct uh_str token = connection->get_header(
        connection, AGENT_LAN_BRIDGE_TOKEN_HEADER);

    if (!config.lan_bootstrap_enabled || source.p == NULL || token.p == NULL ||
        source.len == 0U || source.len >= AGENT_LAN_SESSION_SOURCE_LEN ||
        !agent_invoke_internal_token_matches(
            config.lan_bridge_token, token.p, token.len)) return false;
    memcpy(output, source.p, source.len);
    output[source.len] = '\0';
    if (!source_address_is_trusted_lan(output)) {
        output[0] = '\0';
        return false;
    }
    return true;
}

static bool lan_source_rate_allow(const char *source, uint64_t now_ms)
{
    struct gateway_lan_source_limiter *entry = NULL;
    struct gateway_lan_source_limiter *oldest = NULL;
    size_t index;

    for (index = 0U; index < GATEWAY_LAN_LIMITER_CAPACITY; index++) {
        struct gateway_lan_source_limiter *candidate =
            &runtime.lan_source_limiters[index];
        if (candidate->source_address[0] == '\0') {
            if (entry == NULL) entry = candidate;
            continue;
        }
        if (strcmp(candidate->source_address, source) == 0) {
            entry = candidate;
            break;
        }
        if (oldest == NULL || candidate->last_seen_ms < oldest->last_seen_ms) {
            oldest = candidate;
        }
    }
    if (entry == NULL) entry = oldest;
    if (entry == NULL) return false;
    if (strcmp(entry->source_address, source) != 0) {
        memset(entry, 0, sizeof(*entry));
        if (!copy_text(entry->source_address, sizeof(entry->source_address),
                       source) ||
            !agent_rate_limiter_init(&entry->limiter, 1U, 10U, now_ms)) {
            memset(entry, 0, sizeof(*entry));
            return false;
        }
    }
    entry->last_seen_ms = now_ms;
    return agent_rate_limiter_allow(&entry->limiter, now_ms);
}

static bool lan_source_mark_bootstrap_issued(const char *source)
{
    size_t index;

    for (index = 0U; index < GATEWAY_LAN_LIMITER_CAPACITY; index++) {
        struct gateway_lan_source_limiter *entry =
            &runtime.lan_source_limiters[index];
        if (strcmp(entry->source_address, source) == 0) {
            bool refresh = entry->bootstrap_issued;
            entry->bootstrap_issued = true;
            return refresh;
        }
    }
    return false;
}

static void audit_lan_bootstrap(
    const char *outcome,
    const char *source,
    const char *reason
)
{
    fprintf(stderr, "agent-gw: LAN bootstrap outcome=%s source=%s reason=%s\n",
            outcome, source, reason);
}

static bool capture_public_ingress(
    struct uh_connection *connection,
    struct gateway_request_state *state
)
{
    struct uh_str destination = connection->get_header(
        connection, AGENT_PUBLIC_INGRESS_DEST_HEADER);
    struct uh_str token = connection->get_header(
        connection, AGENT_PUBLIC_INGRESS_TOKEN_HEADER);
    enum agent_public_ingress_result result = agent_public_ingress_validate(
        destination.p, destination.len, token.p, token.len,
        config.public_ingress_token, state->public_ipv6);

    if (result == AGENT_PUBLIC_INGRESS_NONE) return true;
    if (!config.public_ingress_enabled ||
        result != AGENT_PUBLIC_INGRESS_TRUSTED) {
        runtime.public_ingress_rejections++;
        return false;
    }
    state->public_ingress = true;
    (void)copy_text(runtime.last_public_ipv6,
                    sizeof(runtime.last_public_ipv6),
                    state->public_ipv6);
    runtime.public_ingress_requests++;
    return true;
}

static void send_json(
    struct uh_connection *connection,
    int status,
    struct json_object *body
)
{
    const char *serialized;
    size_t length;

    serialized = json_object_to_json_string_ext(
        body, JSON_C_TO_STRING_PLAIN);
    length = strlen(serialized);
    connection->send_head(connection, status, (int64_t)length, NULL);
    connection->send_header(connection, "Content-Type",
                            "application/json; charset=utf-8");
    connection->send_header(connection, "Cache-Control", "no-store");
    connection->send_header(connection, "X-Content-Type-Options", "nosniff");
    connection->end_headers(connection);
    connection->send(connection, serialized, length);
    connection->end_response(connection);
}

static void send_error(
    struct uh_connection *connection,
    int status,
    const char *code,
    const char *message
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *error = json_object_new_object();

    if (root == NULL || error == NULL) {
        if (root != NULL) {
            json_object_put(root);
        }
        if (error != NULL) {
            json_object_put(error);
        }
        connection->send_error(connection, 500, "Out of memory");
        return;
    }
    json_object_object_add(error, "code", json_object_new_string(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(root, "error", error);
    send_json(connection, status, root);
    json_object_put(root);
}

static void destroy_request_state(struct gateway_request_state *state)
{
    struct uh_connection *connection;
    struct ev_loop *loop;
    bool retained;

    if (state == NULL) {
        return;
    }
    connection = state->connection;
    retained = state->connection_retained;
    loop = connection != NULL ? connection->get_loop(connection) : NULL;
    if (loop != NULL) {
        if (ev_is_active(&state->ipc_watcher)) {
            ev_io_stop(loop, &state->ipc_watcher);
        }
        if (ev_is_active(&state->deadline_watcher)) {
            ev_timer_stop(loop, &state->deadline_watcher);
        }
    }
    if (state->ipc_fd >= 0) {
        close(state->ipc_fd);
        state->ipc_fd = -1;
    }
    close_backend_attempt(state);
    if (state->counted && runtime.active_requests > 0U) {
        runtime.active_requests--;
    }
    if (state->tenant_quota_counted) {
        (void)agent_tenant_quota_release(&runtime.tenant_quotas,
                                         state->request.tenant);
    }
    if (connection != NULL && connection->userdata == state) {
        connection->userdata = NULL;
    }
    free(state->forward_body);
    free(state);
    if (retained) {
        connection->decref(connection);
    }
}

static void release_request(struct uh_connection *connection)
{
    struct gateway_request_state *state;

    if (connection->userdata == NULL ||
        connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    state = connection->userdata;
    destroy_request_state(state);
}

static void reject_request(
    struct uh_connection *connection,
    int status,
    const char *code,
    const char *message
)
{
    (void)message;
    connection->send_error(connection, status, "%s", code);
    release_request(connection);
    connection->userdata = CONNECTION_REJECTED;
}

static void connection_closed(struct uh_connection *connection)
{
    struct gateway_request_state *state;

    if (connection->userdata == NULL ||
        connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    state = connection->userdata;
    runtime.client_disconnects++;
    (void)gateway_ipc_machine_transition(
        &state->ipc_machine, GATEWAY_IPC_CLIENT_DISCONNECTED);
    if (state->backend_started) {
        (void)agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_CLIENT_DISCONNECTED);
    }
    if (state->internal_invoke) runtime.internal_invoke_failures++;
    if (state->stream && config.stream_resume_enabled &&
        state->response.route_id[0] != '\0' && state->task_id[0] != '\0' &&
        state->request.tenant[0] != '\0' && state->source_agent[0] != '\0' &&
        state->request.intent[0] != '\0') {
        (void)agent_stream_resume_cache_store(
            &stream_resume_cache, state->task_id, state->request.tenant,
            state->source_agent, state->request.intent,
            state->response.route_id, monotonic_ms());
    }
    destroy_request_state(state);
}

static bool copy_json_string(
    char *target,
    size_t capacity,
    struct json_object *value
)
{
    const char *text;
    size_t length;

    if (value == NULL || !json_object_is_type(value, json_type_string)) {
        return false;
    }
    text = json_object_get_string(value);
    length = (size_t)json_object_get_string_len(value);
    return text != NULL &&
           length > 0U &&
           length < capacity &&
           strlen(text) == length &&
           copy_text(target, capacity, text);
}

static bool get_required_string(
    struct json_object *object,
    const char *name,
    char *target,
    size_t capacity,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;

    if (!json_object_object_get_ex(object, name, &value) ||
        !json_object_is_type(value, json_type_string)) {
        snprintf(error, error_capacity, "%s must be a string", name);
        return false;
    }

    if (!copy_json_string(target, capacity, value)) {
        snprintf(error, error_capacity, "%s is empty or too long", name);
        return false;
    }
    return true;
}

static bool get_required_u32(
    struct json_object *object,
    const char *name,
    uint32_t min_value,
    uint32_t max_value,
    uint32_t *target,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;
    int64_t number;

    if (!json_object_object_get_ex(object, name, &value) ||
        !json_object_is_type(value, json_type_int)) {
        snprintf(error, error_capacity, "%s must be an integer", name);
        return false;
    }
    number = json_object_get_int64(value);
    if (number < (int64_t)min_value ||
        number > (int64_t)max_value) {
        snprintf(error, error_capacity, "%s is outside the allowed range",
                 name);
        return false;
    }
    *target = (uint32_t)number;
    return true;
}

static bool get_optional_nonnegative(
    struct json_object *object,
    const char *name,
    uint64_t *target,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;
    int64_t number;

    if (!json_object_object_get_ex(object, name, &value)) {
        return true;
    }
    if (!json_object_is_type(value, json_type_int)) {
        snprintf(error, error_capacity, "%s must be an integer", name);
        return false;
    }
    number = json_object_get_int64(value);
    if (number < 0) {
        snprintf(error, error_capacity, "%s must not be negative", name);
        return false;
    }
    *target = (uint64_t)number;
    return true;
}

static bool get_region(
    struct json_object *constraints,
    char *target,
    size_t capacity,
    char *error,
    size_t error_capacity
)
{
    struct json_object *region;
    struct json_object *first;

    if (!json_object_object_get_ex(constraints, "region", &region)) {
        target[0] = '\0';
        return true;
    }
    if (json_object_is_type(region, json_type_string)) {
        if (copy_json_string(target, capacity, region)) {
            return true;
        }
        snprintf(error, error_capacity,
                 "constraints.region is empty or too long");
        return false;
    }
    if (json_object_is_type(region, json_type_array) &&
        json_object_array_length(region) == 1U) {
        first = json_object_array_get_idx(region, 0U);
        if (copy_json_string(target, capacity, first)) {
            return true;
        }
    }
    snprintf(error, error_capacity,
             "constraints.region must be a string or one-item string array");
    return false;
}

static bool reject_reserved_envelope_fields(
    struct json_object *root,
    char *error,
    size_t error_capacity
)
{
    json_object_object_foreach(root, name, value) {
        (void)value;
        if (gateway_envelope_field_is_reserved(name)) {
            snprintf(error, error_capacity,
                     "%s is reserved for the authenticated gateway context",
                     name);
            return false;
        }
    }
    return true;
}

static bool parse_envelope(
    const char *body,
    size_t body_length,
    bool invoke,
    struct agent_ipc_lookup_request *request,
    const struct gateway_authenticated_identity *identity,
    bool lan_session,
    bool *identity_mismatch,
    bool *tenant_overridden,
    char task_id[GATEWAY_TASK_ID_LEN],
    char source_agent[AGENT_IPC_AGENT_ID_LEN],
    uint8_t *forwarded_hop_limit_output,
    char **forward_body,
    size_t *forward_body_length,
    uint32_t *effective_backend_timeout_ms,
    bool *interactive_requested,
    char request_deadline[32U],
    bool *retry_requested,
    bool *resume_requested,
    uint64_t *resume_from_event_id,
    bool *deadline_expired,
    char *error,
    size_t error_capacity
)
{
    struct json_tokener *tokener;
    struct json_object *root;
    struct json_object *value;
    struct json_object *constraints;
    enum json_tokener_error json_error;
    uint32_t hop_limit;
    uint32_t trust;
    uint8_t forwarded_hop_limit;
    uint64_t number;
    uint64_t now_epoch_ms;
    char claimed_source_agent[AGENT_IPC_URI_LEN] = {0};
    char idempotency_key[257] = {0};
    bool idempotent = false;
    bool allow_retry = false;
    const char *serialized;
    size_t serialized_length;
    struct json_object *flags;
    enum agent_invoke_deadline_result deadline_result;
    bool valid = false;

    if (body == NULL || request == NULL || identity == NULL ||
        identity_mismatch == NULL ||
        tenant_overridden == NULL || task_id == NULL ||
        source_agent == NULL || forwarded_hop_limit_output == NULL ||
        forward_body == NULL || forward_body_length == NULL ||
        effective_backend_timeout_ms == NULL ||
        interactive_requested == NULL || request_deadline == NULL ||
        retry_requested == NULL ||
        resume_requested == NULL || resume_from_event_id == NULL ||
        deadline_expired == NULL ||
        error == NULL ||
        error_capacity == 0U ||
        body_length == 0U ||
        body_length > (size_t)INT_MAX) {
        if (error != NULL && error_capacity > 0U) {
            snprintf(error, error_capacity,
                     "invalid Envelope parser arguments or body size");
        }
        return false;
    }
    if (!utf8_bytes_valid(body, body_length)) {
        snprintf(error, error_capacity, "body is not valid UTF-8 JSON");
        return false;
    }
    *forward_body = NULL;
    *identity_mismatch = false;
    *forward_body_length = 0U;
    *effective_backend_timeout_ms = config.backend_timeout_ms;
    *interactive_requested = false;
    request_deadline[0] = '\0';
    *retry_requested = false;
    *resume_requested = false;
    *resume_from_event_id = 0U;
    *deadline_expired = false;

    {
        size_t strict_body_length = 0U;
        char *strict_body = strict_json_ascii_copy(
            body, body_length, &strict_body_length);

        if (strict_body == NULL || strict_body_length > (size_t)INT_MAX) {
            free(strict_body);
            snprintf(error, error_capacity, "out of memory");
            return false;
        }
        tokener = json_tokener_new_ex(16);
        if (tokener == NULL) {
            free(strict_body);
            snprintf(error, error_capacity, "out of memory");
            return false;
        }
        json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
        root = json_tokener_parse_ex(
            tokener, strict_body, (int)strict_body_length);
        json_error = json_tokener_get_error(tokener);
        if (json_error == json_tokener_success &&
            json_tokener_get_parse_end(tokener) != strict_body_length) {
            json_error = json_tokener_error_parse_unexpected;
        }
        free(strict_body);
    }
    if (json_error != json_tokener_success) {
        const char *description = json_tokener_error_desc(json_error);
        snprintf(error, error_capacity,
                 strstr(description, "utf-8") != NULL
                    ? "body is not valid UTF-8 JSON: %s"
                    : "body is not valid JSON: %s",
                 description);
        goto done;
    }
    if (root == NULL || !json_object_is_type(root, json_type_object)) {
        snprintf(error, error_capacity,
                 "body is not a complete JSON object");
        goto done;
    }

    if (!json_object_object_get_ex(root, "version", &value) ||
        !json_object_is_type(value, json_type_string) ||
        strcmp(json_object_get_string(value), "1.0") != 0) {
        snprintf(error, error_capacity,
                 "version must be the string '1.0'");
        goto done;
    }
    if (!reject_reserved_envelope_fields(root, error, error_capacity)) {
        goto done;
    }
    if (invoke) {
        if (!json_object_object_get_ex(root, "payload", &value) ||
            json_object_is_type(value, json_type_null)) {
            snprintf(error, error_capacity,
                     "payload is required by the invoke endpoint");
            goto done;
        }
    } else if (json_object_object_get_ex(root, "payload", &value)) {
        snprintf(error, error_capacity,
                 "payload is not accepted by the route-only endpoint");
        goto done;
    }

    memset(request, 0, sizeof(*request));
    request->request_id = next_request_id++;
    if (next_request_id == 0U) {
        next_request_id = 1U;
    }

    if (!get_required_string(root, "intent", request->intent,
                             sizeof(request->intent),
                             error, error_capacity) ||
        !get_required_u32(root, "intent_version", 1U, UINT32_MAX,
                          &request->version, error, error_capacity) ||
        !get_required_string(root, "task_id", task_id,
                             GATEWAY_TASK_ID_LEN,
                             error, error_capacity) ||
        !get_required_u32(root, "hop_limit", 1U, 255U, &hop_limit,
                          error, error_capacity)) {
        goto done;
    }
    if (invoke && !agent_invoke_decrement_hop_limit(
                      hop_limit, &forwarded_hop_limit)) {
        snprintf(error, error_capacity,
                 "hop_limit would be exhausted by this router");
        goto done;
    }

    if (json_object_object_get_ex(root, "tenant", &value)) {
        if (!copy_json_string(request->tenant, sizeof(request->tenant),
                              value)) {
            snprintf(error, error_capacity,
                     "tenant is empty, too long or not a string");
            goto done;
        }
    } else if (!identity->verified) {
        snprintf(error, error_capacity,
                 "tenant is required until identity binding is enabled");
        goto done;
    }
    if (json_object_object_get_ex(root, "source_agent", &value) &&
        !copy_json_string(claimed_source_agent,
                          sizeof(claimed_source_agent), value)) {
        snprintf(error, error_capacity,
                 "source_agent is empty, too long or not a string");
        goto done;
    }
    if (json_object_object_get_ex(root, "target_agent", &value) &&
        !copy_json_string(request->target_agent,
                          sizeof(request->target_agent), value)) {
        snprintf(error, error_capacity,
                 "target_agent is empty, too long or not a string");
        goto done;
    }
    if (invoke && !identity->verified && claimed_source_agent[0] == '\0') {
        snprintf(error, error_capacity,
                 "source_agent is required without identity binding");
        goto done;
    }
    if (lan_session &&
        ((request->tenant[0] != '\0' &&
          strcmp(request->tenant, identity->tenant) != 0) ||
         (claimed_source_agent[0] != '\0' &&
          strcmp(claimed_source_agent, identity->source_agent) != 0 &&
          !(strncmp(identity->source_agent, "agent://", 8U) == 0 &&
            strrchr(identity->source_agent, '/') != NULL &&
            strcmp(strrchr(identity->source_agent, '/') + 1U,
                   claimed_source_agent) == 0)))) {
        *identity_mismatch = true;
        snprintf(error, error_capacity,
                 "LAN session identity does not match the Envelope");
        goto done;
    }
    if (!gateway_apply_authenticated_identity(
            request, identity, tenant_overridden)) {
        snprintf(error, error_capacity,
                 "authenticated identity claims are incomplete");
        goto done;
    }
    if ((identity->verified || claimed_source_agent[0] != '\0') &&
        !copy_text(source_agent, AGENT_IPC_AGENT_ID_LEN,
                   identity->verified ? identity->source_agent :
                                        claimed_source_agent)) {
        snprintf(error, error_capacity,
                 "source_agent exceeds Relay invoke bound");
        goto done;
    }
    if (invoke) *forwarded_hop_limit_output = forwarded_hop_limit;

    if (invoke) {
        if (json_object_object_get_ex(root, "resume_from_event_id", &value)) {
            int64_t cursor;

            if (!json_object_is_type(value, json_type_int) ||
                (cursor = json_object_get_int64(value)) < 0) {
                snprintf(error, error_capacity,
                         "resume_from_event_id must be a non-negative integer");
                goto done;
            }
            *resume_requested = cursor > 0;
            *resume_from_event_id = (uint64_t)cursor;
        }
        if (json_object_object_get_ex(root, "flags", &flags)) {
            if (!json_object_is_type(flags, json_type_object)) {
                snprintf(error, error_capacity, "flags must be an object");
                goto done;
            }
            if (json_object_object_get_ex(flags, "idempotent", &value)) {
                if (!json_object_is_type(value, json_type_boolean)) {
                    snprintf(error, error_capacity,
                             "flags.idempotent must be boolean");
                    goto done;
                }
                idempotent = json_object_get_boolean(value);
            }
            if (json_object_object_get_ex(flags, "allow_retry", &value)) {
                if (!json_object_is_type(value, json_type_boolean)) {
                    snprintf(error, error_capacity,
                             "flags.allow_retry must be boolean");
                    goto done;
                }
                allow_retry = json_object_get_boolean(value);
            }
            if (json_object_object_get_ex(flags, "interactive", &value)) {
                if (!json_object_is_type(value, json_type_boolean)) {
                    snprintf(error, error_capacity,
                             "flags.interactive must be boolean");
                    goto done;
                }
                *interactive_requested = json_object_get_boolean(value);
            }
        }
        if (json_object_object_get_ex(root, "idempotency_key", &value) &&
            !copy_json_string(idempotency_key,
                              sizeof(idempotency_key), value)) {
            snprintf(error, error_capacity,
                     "idempotency_key is empty, too long or not a string");
            goto done;
        }
        if (!agent_invoke_retry_declaration_valid(
                idempotent, allow_retry,
                idempotency_key[0] != '\0' ? idempotency_key : NULL)) {
            snprintf(error, error_capacity,
                     "allow_retry requires idempotent and idempotency_key");
            goto done;
        }
        *retry_requested = allow_retry;
        if (json_object_object_get_ex(root, "deadline", &value) &&
            !copy_json_string(request_deadline, 32U, value)) {
            snprintf(error, error_capacity,
                     "deadline must be a supported UTC timestamp");
            goto done;
        }
        now_epoch_ms = wall_clock_seconds() * 1000U;
        deadline_result = agent_invoke_resolve_timeout_ms(
            now_epoch_ms, config.backend_timeout_ms,
            request_deadline[0] != '\0' ? request_deadline : NULL,
            effective_backend_timeout_ms);
        if (deadline_result != AGENT_INVOKE_DEADLINE_OK) {
            *deadline_expired =
                deadline_result == AGENT_INVOKE_DEADLINE_EXPIRED;
            snprintf(error, error_capacity, "%s",
                     *deadline_expired ? "deadline has expired" :
                                         "deadline is invalid");
            goto done;
        }
        json_object_object_add(
            root, "tenant", json_object_new_string(request->tenant));
        if (identity->verified) {
            json_object_object_add(
                root, "source_agent",
                json_object_new_string(identity->source_agent));
        }
        json_object_object_add(
            root, "hop_limit",
            json_object_new_int((int)forwarded_hop_limit));
    }

    if (!json_object_object_get_ex(root, "constraints", &constraints)) {
        valid = true;
        goto serialize;
    }
    if (!json_object_is_type(constraints, json_type_object)) {
        snprintf(error, error_capacity, "constraints must be an object");
        goto done;
    }

    if (!get_region(constraints, request->region,
                    sizeof(request->region), error, error_capacity) ||
        !get_optional_nonnegative(constraints, "max_cost_microunits",
                                  &request->max_cost_microunits,
                                  error, error_capacity)) {
        goto done;
    }

    number = 0U;
    if (!get_optional_nonnegative(constraints, "max_latency_ms", &number,
                                  error, error_capacity) ||
        number > UINT32_MAX) {
        if (error[0] == '\0') {
            snprintf(error, error_capacity,
                     "max_latency_ms exceeds uint32");
        }
        goto done;
    }
    request->max_latency_ms = (uint32_t)number;

    trust = 0U;
    if (json_object_object_get_ex(constraints, "min_trust_level", &value)) {
        if (!get_required_u32(constraints, "min_trust_level", 0U, 100U,
                              &trust, error, error_capacity)) {
            goto done;
        }
    }
    request->min_trust_level = (uint8_t)trust;
    valid = true;

serialize:
    if (valid && invoke) {
        serialized = json_object_to_json_string_ext(
            root, JSON_C_TO_STRING_PLAIN);
        serialized_length = strlen(serialized);
        *forward_body = malloc(serialized_length + 1U);
        if (*forward_body == NULL) {
            snprintf(error, error_capacity, "out of memory");
            valid = false;
        } else {
            memcpy(*forward_body, serialized, serialized_length + 1U);
            *forward_body_length = serialized_length;
        }
    }

done:
    if (root != NULL) {
        json_object_put(root);
    }
    json_tokener_free(tokener);
    return valid;
}

static bool json_copy_required_string(
    struct json_object *root,
    const char *name,
    char *target,
    size_t capacity,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;

    if (!json_object_object_get_ex(root, name, &value) ||
        !json_object_is_type(value, json_type_string) ||
        !copy_text(target, capacity, json_object_get_string(value))) {
        snprintf(error, error_capacity, "%s must be a non-empty bounded string",
                 name);
        return false;
    }
    return true;
}

static bool json_optional_u64(
    struct json_object *root,
    const char *name,
    uint64_t *target,
    uint64_t maximum,
    bool *present,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;
    int64_t number;

    *present = false;
    if (!json_object_object_get_ex(root, name, &value)) return true;
    if (!json_object_is_type(value, json_type_int)) {
        snprintf(error, error_capacity, "%s must be an integer", name);
        return false;
    }
    number = json_object_get_int64(value);
    if (number < 0 || (uint64_t)number > maximum) {
        snprintf(error, error_capacity, "%s is outside the supported range",
                 name);
        return false;
    }
    *target = (uint64_t)number;
    *present = true;
    return true;
}

static bool parse_registration_backend_tls(
    struct json_object *root,
    const char *registered_endpoint,
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
)
{
    struct json_object *descriptor;
    char address[46U];
    char identity[GATEWAY_TLS_IDENTITY_LEN];
    char mapping[AGENT_INVOKE_REMOTE_MAP_LEN];
    char endpoint[AGENT_IPC_URI_LEN];
    uint64_t port;
    bool present;
    int written;

    if (!json_object_object_get_ex(root, "backend_tls", &descriptor)) {
        return true;
    }
    if (!config.remote_backend_enabled) {
        snprintf(error, error_capacity,
                 "automatic HTTPS Agent registration is disabled");
        return false;
    }
    if (!json_object_is_type(descriptor, json_type_object) ||
        !json_copy_required_string(descriptor, "address", address,
                                   sizeof(address), error, error_capacity) ||
        !json_copy_required_string(descriptor, "tls_server_name", identity,
                                   sizeof(identity), error, error_capacity) ||
        !json_copy_required_string(
            descriptor, "ca_bundle_id", state->registration_ca_bundle_id,
            sizeof(state->registration_ca_bundle_id), error, error_capacity) ||
        !json_copy_required_string(
            descriptor, "certificate_sha256",
            state->registration_certificate_sha256,
            sizeof(state->registration_certificate_sha256), error,
            error_capacity) ||
        !json_optional_u64(descriptor, "port", &port, UINT16_MAX,
                           &present, error, error_capacity) ||
        !present || port == 0U) {
        if (error[0] == '\0') {
            snprintf(error, error_capacity,
                     "backend_tls needs address, port, TLS name, trusted CA ID and certificate fingerprint");
        }
        return false;
    }
    if (strcmp(state->registration_ca_bundle_id,
               config.remote_backend_ca_bundle_id) != 0) {
        snprintf(error, error_capacity,
                 "backend_tls CA bundle is not trusted by this router");
        return false;
    }
    written = snprintf(endpoint, sizeof(endpoint), "https://%s:%u/",
                       identity, (unsigned int)port);
    if (written < 0 || (size_t)written >= sizeof(endpoint) ||
        !agent_invoke_parse_remote_tls_endpoint(
            endpoint, &state->registration_tls_endpoint)) {
        snprintf(error, error_capacity, "backend_tls identity or port is invalid");
        return false;
    }
    if (strchr(address, ':') != NULL) {
        written = snprintf(mapping, sizeof(mapping), "%s:%u=[%s]",
                           identity, (unsigned int)port, address);
    } else {
        written = snprintf(mapping, sizeof(mapping), "%s:%u=%s",
                           identity, (unsigned int)port, address);
    }
    if (written < 0 || (size_t)written >= sizeof(mapping) ||
        !agent_invoke_parse_remote_map(
            mapping, &state->registration_tls_map)) {
        snprintf(error, error_capacity,
                 "backend_tls address is not an allowed numeric unicast address");
        return false;
    }
    if (registered_endpoint != NULL) {
        struct agent_invoke_endpoint parsed;
        if (!agent_invoke_parse_remote_tls_endpoint(
                registered_endpoint, &parsed) ||
            !agent_invoke_remote_map_matches(
                &parsed, &state->registration_tls_map)) {
            snprintf(error, error_capacity,
                     "backend_tls identity and port must match endpoint");
            return false;
        }
        /* The backend identity descriptor binds authority and certificate,
         * while the registered endpoint also carries the executable path.
         * Preserve that full endpoint in the immutable lease snapshot; using
         * the descriptor's synthetic "/" path makes a legitimate /invoke
         * route impossible to match later. */
        state->registration_tls_endpoint = parsed;
        state->registration_backend_tls_endpoint_bound = true;
    }
    state->registration_backend_tls = true;
    return true;
}

static bool json_optional_boolean(
    struct json_object *root,
    const char *name,
    bool default_value,
    bool *target,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;

    *target = default_value;
    if (!json_object_object_get_ex(root, name, &value)) return true;
    if (!json_object_is_type(value, json_type_boolean)) {
        snprintf(error, error_capacity, "%s must be boolean", name);
        return false;
    }
    *target = json_object_get_boolean(value);
    return true;
}

static bool json_copy_optional_string(
    struct json_object *root,
    const char *name,
    char *target,
    size_t capacity,
    char *error,
    size_t error_capacity
)
{
    struct json_object *value;

    target[0] = '\0';
    if (!json_object_object_get_ex(root, name, &value)) return true;
    if (!json_object_is_type(value, json_type_string) ||
        !copy_text(target, capacity, json_object_get_string(value))) {
        snprintf(error, error_capacity,
                 "%s must be a non-empty bounded string", name);
        return false;
    }
    return true;
}

static bool lowercase_sha256(const char *value)
{
    size_t index;

    if (value == NULL || strlen(value) != 64U) return false;
    for (index = 0U; index < 64U; index++) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

static bool parse_registration_computer(
    struct json_object *cloud,
    struct agent_ipc_register_request *request,
    char *error,
    size_t error_capacity
)
{
    static const struct {
        const char *name;
        uint16_t bit;
    } scopes[] = {
        {"connection.list", AGENT_IPC_WORKSPACE_CONNECTION_LIST},
        {"connection.create", AGENT_IPC_WORKSPACE_CONNECTION_CREATE},
        {"connection.update", AGENT_IPC_WORKSPACE_CONNECTION_UPDATE},
        {"connection.delete", AGENT_IPC_WORKSPACE_CONNECTION_DELETE},
        {"connection.test", AGENT_IPC_WORKSPACE_CONNECTION_TEST},
        {"connection.bind", AGENT_IPC_WORKSPACE_CONNECTION_BIND},
        {"files.list", AGENT_IPC_WORKSPACE_FILES_LIST},
        {"files.read", AGENT_IPC_WORKSPACE_FILES_READ},
        {"files.write", AGENT_IPC_WORKSPACE_FILES_WRITE},
        {"command.execute", AGENT_IPC_WORKSPACE_COMMAND_EXECUTE},
        {"browser.control", AGENT_IPC_WORKSPACE_BROWSER_CONTROL},
    };
    struct json_object *computer;
    struct json_object *requirement;
    struct json_object *capabilities;
    size_t index;
    size_t item_index;

    if (!json_object_object_get_ex(cloud, "computer", &computer)) return true;
    if (!json_object_is_type(computer, json_type_object) ||
        !json_object_object_get_ex(computer, "requirement", &requirement) ||
        !json_object_is_type(requirement, json_type_string) ||
        !json_object_object_get_ex(
            computer, "workspace_capabilities", &capabilities) ||
        !json_object_is_type(capabilities, json_type_array)) {
        snprintf(error, error_capacity,
                 "cloud.computer requires requirement and workspace_capabilities");
        return false;
    }
    request->cloud.computer_present = true;
    if (strcmp(json_object_get_string(requirement), "disabled") == 0) {
        request->cloud.computer_requirement = AGENT_IPC_COMPUTER_DISABLED;
    } else if (strcmp(json_object_get_string(requirement), "optional") == 0) {
        request->cloud.computer_requirement = AGENT_IPC_COMPUTER_OPTIONAL;
    } else if (strcmp(json_object_get_string(requirement), "required") == 0) {
        request->cloud.computer_requirement = AGENT_IPC_COMPUTER_REQUIRED;
    } else {
        snprintf(error, error_capacity,
                 "cloud.computer.requirement is unsupported");
        return false;
    }
    for (item_index = 0U;
         item_index < json_object_array_length(capabilities); item_index++) {
        struct json_object *item =
            json_object_array_get_idx(capabilities, item_index);
        bool matched = false;

        if (!json_object_is_type(item, json_type_string)) {
            snprintf(error, error_capacity,
                     "cloud.computer.workspace_capabilities must contain strings");
            return false;
        }
        for (index = 0U; index < sizeof(scopes) / sizeof(scopes[0]); index++) {
            if (strcmp(json_object_get_string(item), scopes[index].name) != 0)
                continue;
            request->cloud.workspace_capabilities |= scopes[index].bit;
            matched = true;
            break;
        }
        if (!matched) {
            snprintf(error, error_capacity,
                     "cloud.computer.workspace_capabilities contains unsupported scope");
            return false;
        }
    }
    if (request->cloud.computer_requirement == AGENT_IPC_COMPUTER_DISABLED &&
        request->cloud.workspace_capabilities != 0U) {
        snprintf(error, error_capacity,
                 "disabled cloud.computer cannot declare Workspace capabilities");
        return false;
    }
    return true;
}

static bool parse_mobile_scopes(
    struct json_object *array,
    uint16_t *target,
    const char *field,
    char *error,
    size_t error_capacity
)
{
    static const struct {
        const char *name;
        uint16_t bit;
    } scopes[] = {
        {"mobile.observe", AGENT_IPC_MOBILE_OBSERVE},
        {"mobile.screen.capture", AGENT_IPC_MOBILE_SCREEN_CAPTURE},
        {"mobile.tap", AGENT_IPC_MOBILE_TAP},
        {"mobile.type_text", AGENT_IPC_MOBILE_TYPE_TEXT},
        {"mobile.swipe", AGENT_IPC_MOBILE_SWIPE},
        {"mobile.press_back", AGENT_IPC_MOBILE_PRESS_BACK},
        {"mobile.open_app", AGENT_IPC_MOBILE_OPEN_APP},
        {"mobile.wait_for_state", AGENT_IPC_MOBILE_WAIT_FOR_STATE},
    };
    size_t item_index;

    if (!json_object_is_type(array, json_type_array)) {
        snprintf(error, error_capacity, "%s must be an array", field);
        return false;
    }
    for (item_index = 0U; item_index < json_object_array_length(array);
         item_index++) {
        struct json_object *item = json_object_array_get_idx(array, item_index);
        size_t index;
        bool matched = false;

        if (!json_object_is_type(item, json_type_string)) {
            snprintf(error, error_capacity, "%s must contain strings", field);
            return false;
        }
        for (index = 0U; index < sizeof(scopes) / sizeof(scopes[0]); index++) {
            if (strcmp(json_object_get_string(item), scopes[index].name) != 0)
                continue;
            *target |= scopes[index].bit;
            matched = true;
            break;
        }
        if (!matched) {
            snprintf(error, error_capacity, "%s contains unsupported scope", field);
            return false;
        }
    }
    return true;
}

static bool parse_registration_mobile(
    struct json_object *cloud,
    struct agent_ipc_register_request *request,
    char *error,
    size_t error_capacity
)
{
    struct json_object *mobile;
    struct json_object *requirement;
    struct json_object *capabilities;
    const char *value;

    if (!json_object_object_get_ex(cloud, "mobile", &mobile)) return true;
    if (!json_object_is_type(mobile, json_type_object) ||
        !json_object_object_get_ex(mobile, "requirement", &requirement) ||
        !json_object_is_type(requirement, json_type_string) ||
        !json_object_object_get_ex(
            mobile, "mobile_capabilities", &capabilities)) {
        snprintf(error, error_capacity,
                 "cloud.mobile requires requirement and mobile_capabilities");
        return false;
    }
    request->cloud.mobile_present = true;
    value = json_object_get_string(requirement);
    if (strcmp(value, "disabled") == 0) {
        request->cloud.mobile_requirement = AGENT_IPC_MOBILE_DISABLED;
    } else if (strcmp(value, "optional") == 0) {
        request->cloud.mobile_requirement = AGENT_IPC_MOBILE_OPTIONAL;
    } else if (strcmp(value, "required") == 0) {
        request->cloud.mobile_requirement = AGENT_IPC_MOBILE_REQUIRED;
    } else {
        snprintf(error, error_capacity,
                 "cloud.mobile.requirement is unsupported");
        return false;
    }
    if (!parse_mobile_scopes(
            capabilities, &request->cloud.mobile_capabilities,
            "cloud.mobile.mobile_capabilities", error, error_capacity))
        return false;
    if (request->cloud.mobile_requirement == AGENT_IPC_MOBILE_DISABLED &&
        request->cloud.mobile_capabilities != 0U) {
        snprintf(error, error_capacity,
                 "disabled cloud.mobile cannot declare Mobile capabilities");
        return false;
    }
    if (request->cloud.mobile_requirement == AGENT_IPC_MOBILE_REQUIRED &&
        request->cloud.mobile_capabilities == 0U) {
        snprintf(error, error_capacity,
                 "required cloud.mobile must declare Mobile capabilities");
        return false;
    }
    return true;
}

static bool parse_registration_cloud(
    struct json_object *root,
    struct agent_ipc_register_request *request,
    char *error,
    size_t error_capacity
)
{
    struct json_object *cloud;
    struct json_object *tool;
    struct json_object *schema;
    struct json_object *value;
    const char *serialized;
    uint64_t intent_version;
    uint64_t recovery_protocol = 0U;
    bool present;
    bool resumable_present;
    bool continuable_present;

    if (!json_object_object_get_ex(root, "cloud", &cloud)) return true;
    if (!json_object_is_type(cloud, json_type_object)) {
        snprintf(error, error_capacity, "cloud must be an object");
        return false;
    }
    request->cloud.present = true;
    if (!json_optional_boolean(cloud, "publish", false,
                               &request->cloud.publish,
                               error, error_capacity) ||
        !json_copy_required_string(
            cloud, "agent_name", request->cloud.agent_name,
            sizeof(request->cloud.agent_name), error, error_capacity) ||
        !json_copy_required_string(
            cloud, "manifest_digest", request->cloud.manifest_digest,
            sizeof(request->cloud.manifest_digest), error, error_capacity) ||
        !lowercase_sha256(request->cloud.manifest_digest)) {
        if (error[0] == '\0') {
            snprintf(error, error_capacity,
                     "cloud.manifest_digest must be lowercase SHA-256");
        }
        return false;
    }
    if (!parse_registration_computer(
            cloud, request, error, error_capacity)) return false;
    if (!parse_registration_mobile(
            cloud, request, error, error_capacity)) return false;
    if (!json_object_object_get_ex(cloud, "tool", &tool)) return true;
    if (!json_object_is_type(tool, json_type_object)) {
        snprintf(error, error_capacity, "cloud.tool must be an object");
        return false;
    }
    request->cloud.tool_present = true;
    if (!json_copy_required_string(
            tool, "name", request->cloud.tool_name,
            sizeof(request->cloud.tool_name), error, error_capacity) ||
        !json_copy_optional_string(
            tool, "title", request->cloud.tool_title,
            sizeof(request->cloud.tool_title), error, error_capacity) ||
        !json_copy_optional_string(
            tool, "description", request->cloud.tool_description,
            sizeof(request->cloud.tool_description), error, error_capacity) ||
        !json_copy_required_string(
            tool, "intent", request->cloud.tool_description,
            sizeof(request->cloud.tool_description), error, error_capacity) ||
        strcmp(request->cloud.tool_description, request->intent) != 0 ||
        !json_optional_u64(tool, "intent_version", &intent_version,
                           UINT32_MAX, &present, error, error_capacity) ||
        !present || intent_version != request->version) {
        if (error[0] == '\0') {
            snprintf(error, error_capacity,
                     "cloud.tool intent must match the registered capability");
        }
        return false;
    }
    /* The temporary intent comparison buffer reused description storage;
     * restore the actual optional description after identity validation. */
    if (!json_copy_optional_string(
            tool, "description", request->cloud.tool_description,
            sizeof(request->cloud.tool_description), error, error_capacity))
        return false;
    if (!json_object_object_get_ex(tool, "input_schema", &schema) ||
        !json_object_is_type(schema, json_type_object)) {
        snprintf(error, error_capacity,
                 "cloud.tool.input_schema must be an object");
        return false;
    }
    serialized = json_object_to_json_string_ext(
        schema, JSON_C_TO_STRING_PLAIN);
    if (serialized == NULL ||
        !copy_text(request->cloud.tool_input_schema,
                   sizeof(request->cloud.tool_input_schema), serialized)) {
        snprintf(error, error_capacity,
                 "cloud.tool.input_schema exceeds the adapter bound");
        return false;
    }
    resumable_present = json_object_object_get_ex(
        tool, "resumable", &value);
    continuable_present = json_object_object_get_ex(
        tool, "continuable", &value);
    if (!json_optional_boolean(tool, "task", false, &request->cloud.task,
                               error, error_capacity) ||
        !json_optional_boolean(tool, "resumable", false,
                               &request->cloud.resumable,
                               error, error_capacity) ||
        !json_optional_boolean(tool, "continuable",
                               request->cloud.resumable,
                               &request->cloud.continuable,
                               error, error_capacity) ||
        !json_optional_u64(tool, "recovery_protocol", &recovery_protocol,
                           1U, &present, error, error_capacity) ||
        !json_optional_boolean(tool, "demo", false, &request->cloud.demo,
                               error, error_capacity) ||
        !json_optional_boolean(tool, "chat", false, &request->cloud.chat,
                               error, error_capacity) ||
        !json_optional_boolean(tool, "interactive", false,
                               &request->cloud.interactive,
                               error, error_capacity) ||
        (resumable_present && continuable_present &&
         request->cloud.resumable != request->cloud.continuable) ||
        (request->cloud.continuable && !request->cloud.task) ||
        (request->cloud.chat &&
         (!request->cloud.task || !request->cloud.interactive))) {
        if (error[0] == '\0') {
            snprintf(error, error_capacity,
                     "cloud.tool execution policy is inconsistent");
        }
        return false;
    }
    request->cloud.recovery_protocol = (uint8_t)recovery_protocol;
    if (json_object_object_get_ex(tool, "mobile_scopes", &value)) {
        if (!parse_mobile_scopes(
                value, &request->cloud.mobile_scopes,
                "cloud.tool.mobile_scopes", error, error_capacity))
            return false;
        if (!request->cloud.mobile_present ||
            (request->cloud.mobile_scopes &
             (uint16_t)~request->cloud.mobile_capabilities) != 0U) {
            snprintf(error, error_capacity,
                     "cloud.tool.mobile_scopes must be declared by cloud.mobile");
            return false;
        }
    }
    if (json_object_object_get_ex(tool, "output_schema", &value) &&
        !json_object_is_type(value, json_type_object)) {
        snprintf(error, error_capacity,
                 "cloud.tool.output_schema must be an object when present");
        return false;
    }
    return true;
}

static bool parse_registration_operation(
    const char *body,
    size_t body_length,
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
)
{
    struct json_tokener *tokener = json_tokener_new();
    struct json_object *root = NULL;
    struct json_object *value;
    uint64_t number;
    bool present;
    bool valid = false;

    if (tokener == NULL) {
        snprintf(error, error_capacity, "out of memory");
        return false;
    }
    root = json_tokener_parse_ex(tokener, body, (int)body_length);
    if (root == NULL || json_tokener_get_error(tokener) != json_tokener_success ||
        !json_object_is_type(root, json_type_object)) {
        snprintf(error, error_capacity, "body must be one JSON object");
        goto done;
    }
    state->request.request_id = next_request_id++;
    if (next_request_id == 0U) next_request_id = 1U;
    if (state->operation == GATEWAY_OPERATION_REGISTER) {
        struct agent_ipc_register_request *request = &state->registration;

        memset(request, 0, sizeof(*request));
        request->request_id = state->request.request_id;
        request->trust_level = 50U;
        if (!json_copy_required_string(root, "intent", request->intent,
                                       sizeof(request->intent), error,
                                       error_capacity) ||
            !json_copy_required_string(root, "origin", request->origin,
                                       sizeof(request->origin), error,
                                       error_capacity) ||
            !json_copy_required_string(root, "endpoint", request->endpoint,
                                       sizeof(request->endpoint), error,
                                       error_capacity) ||
            !json_copy_required_string(root, "tenant", request->tenant,
                                       sizeof(request->tenant), error,
                                       error_capacity)) goto done;
        if (json_object_object_get_ex(root, "route_id", &value)) {
            if (!json_object_is_type(value, json_type_string) ||
                !copy_text(request->route_id, sizeof(request->route_id),
                           json_object_get_string(value))) {
                snprintf(error, error_capacity, "route_id is invalid");
                goto done;
            }
        }
        if (json_object_object_get_ex(root, "region", &value)) {
            if (!json_object_is_type(value, json_type_string) ||
                !copy_text(request->region, sizeof(request->region),
                           json_object_get_string(value))) {
                snprintf(error, error_capacity, "region is invalid");
                goto done;
            }
        } else {
            (void)copy_text(request->region, sizeof(request->region), "local");
        }
        if (!json_optional_u64(root, "version", &number, UINT32_MAX,
                               &present, error, error_capacity)) goto done;
        request->version = present ? (uint32_t)number : 1U;
        if (request->version == 0U) {
            snprintf(error, error_capacity, "version must be greater than zero");
            goto done;
        }
        if (!json_optional_u64(root, "lease_seconds", &number, UINT32_MAX,
                               &present, error, error_capacity)) goto done;
        request->lease_seconds = present ? (uint32_t)number : 0U;
        if (!json_optional_u64(root, "cost_microunits", &number, UINT64_MAX,
                               &present, error, error_capacity)) goto done;
        request->cost_microunits = present ? number : 0U;
        if (!json_optional_u64(root, "latency_ms", &number, UINT32_MAX,
                               &present, error, error_capacity)) goto done;
        request->latency_ms = present ? (uint32_t)number : 0U;
        if (!json_optional_u64(root, "trust", &number, 100U,
                               &present, error, error_capacity)) goto done;
        request->trust_level = present ? (uint8_t)number : 50U;
        if (!json_optional_u64(root, "load_permille", &number, 1000U,
                               &present, error, error_capacity)) goto done;
        request->load_permille = present ? (uint16_t)number : 0U;
        if (!json_optional_u64(root, "hop_count", &number, UINT8_MAX,
                               &present, error, error_capacity)) goto done;
        request->hop_count = present ? (uint8_t)number : 0U;
        if (json_object_object_get_ex(root, "public_ipv6", &value)) {
            if (!json_object_is_type(value, json_type_string) ||
                strcmp(json_object_get_string(value), "auto") != 0) {
                snprintf(error, error_capacity,
                         "public_ipv6 must be the string auto");
                goto done;
            }
            request->request_public_ipv6 = true;
        }
        if (!parse_registration_backend_tls(
                root, request->endpoint, state, error, error_capacity)) {
            goto done;
        }
        if (!parse_registration_cloud(
                root, request, error, error_capacity)) goto done;
        if (state->identity.verified) {
            if (state->lan_session &&
                (strcmp(request->tenant, state->identity.tenant) != 0 ||
                 strcmp(request->origin,
                        state->identity.source_agent) != 0)) {
                state->lan_identity_mismatch = true;
                snprintf(error, error_capacity,
                         "LAN session identity does not match registration");
                goto done;
            }
            state->tenant_overridden =
                strcmp(request->tenant, state->identity.tenant) != 0;
            if (!copy_text(request->tenant, sizeof(request->tenant),
                           state->identity.tenant) ||
                !copy_text(request->origin, sizeof(request->origin),
                           state->identity.source_agent)) {
                snprintf(error, error_capacity,
                         "authenticated identity exceeds registration bounds");
                goto done;
            }
        }
    } else if (state->operation == GATEWAY_OPERATION_RENEW) {
        struct agent_ipc_renew_request *request = &state->renewal;
        char renewal_endpoint[AGENT_IPC_URI_LEN] = "";
        const char *registered_endpoint = NULL;

        memset(request, 0, sizeof(*request));
        request->request_id = state->request.request_id;
        if (!json_copy_required_string(root, "route_id", request->route_id,
                                       sizeof(request->route_id), error,
                                       error_capacity)) goto done;
        if (!json_optional_u64(root, "lease_seconds", &number, UINT32_MAX,
                               &present, error, error_capacity)) goto done;
        request->lease_seconds = present ? (uint32_t)number : 0U;
        if (!json_optional_u64(root, "latency_ms", &number, UINT32_MAX,
                               &present, error, error_capacity)) goto done;
        if (present) {
            request->latency_ms = (uint32_t)number;
            request->update_flags |= AGENT_IPC_RENEW_LATENCY;
        }
        if (!json_optional_u64(root, "load_permille", &number, 1000U,
                               &present, error, error_capacity)) goto done;
        if (present) {
            request->load_permille = (uint16_t)number;
            request->update_flags |= AGENT_IPC_RENEW_LOAD;
        }
        if (json_object_object_get_ex(root, "healthy", &value)) {
            if (!json_object_is_type(value, json_type_boolean)) {
                snprintf(error, error_capacity, "healthy must be boolean");
                goto done;
            }
            request->healthy = json_object_get_boolean(value);
            request->update_flags |= AGENT_IPC_RENEW_HEALTH;
        }
        if (json_object_object_get_ex(root, "endpoint", &value)) {
            if (!json_object_is_type(value, json_type_string) ||
                !copy_text(renewal_endpoint, sizeof(renewal_endpoint),
                           json_object_get_string(value))) {
                snprintf(error, error_capacity,
                         "endpoint must be a non-empty bounded string");
                goto done;
            }
            registered_endpoint = renewal_endpoint;
        }
        if (!parse_registration_backend_tls(
                root, registered_endpoint, state, error, error_capacity)) {
            goto done;
        }
        if (state->identity.verified &&
            !copy_text(request->requester_agent,
                       sizeof(request->requester_agent),
                       state->identity.source_agent)) {
            snprintf(error, error_capacity,
                     "authenticated identity exceeds renewal bounds");
            goto done;
        }
    } else {
        memset(&state->removal, 0, sizeof(state->removal));
        state->removal.request_id = state->request.request_id;
        if (!json_copy_required_string(root, "route_id",
                                       state->removal.route_id,
                                       sizeof(state->removal.route_id), error,
                                       error_capacity)) goto done;
        if (state->identity.verified &&
            !copy_text(state->removal.requester_agent,
                       sizeof(state->removal.requester_agent),
                       state->identity.source_agent)) {
            snprintf(error, error_capacity,
                     "authenticated identity exceeds unregister bounds");
            goto done;
        }
    }
    valid = true;

done:
    if (root != NULL) json_object_put(root);
    json_tokener_free(tokener);
    return valid;
}

static bool operation_is_registration(enum gateway_operation operation)
{
    return operation == GATEWAY_OPERATION_REGISTER ||
           operation == GATEWAY_OPERATION_RENEW ||
           operation == GATEWAY_OPERATION_UNREGISTER;
}

static uint64_t lease_expiry_ms(uint64_t now_ms, uint32_t lease_seconds)
{
    const uint64_t lease_ms = (uint64_t)lease_seconds * 1000U;
    return UINT64_MAX - now_ms < lease_ms ? UINT64_MAX : now_ms + lease_ms;
}

static bool synchronize_registration_backend_map(
    struct gateway_request_state *state
)
{
    const uint64_t now_ms = monotonic_ms();
    const uint64_t expires_at_ms = lease_expiry_ms(
        now_ms, state->lease_response.lease_seconds);

    if (state->operation == GATEWAY_OPERATION_UNREGISTER) {
        (void)agent_invoke_dynamic_map_remove(
            &dynamic_backend_maps, state->removal.route_id);
        return true;
    }
    if (state->operation == GATEWAY_OPERATION_REGISTER &&
        state->registration_backend_tls) {
        return agent_invoke_dynamic_map_upsert(
            &dynamic_backend_maps, state->lease_response.route_id,
            &state->registration_tls_endpoint,
            &state->registration_tls_map,
            state->registration_ca_bundle_id,
            state->registration_certificate_sha256,
            expires_at_ms, now_ms);
    }
    if (state->operation == GATEWAY_OPERATION_RENEW) {
        if (state->registration_backend_tls &&
            state->registration_backend_tls_endpoint_bound) {
            return agent_invoke_dynamic_map_upsert(
                &dynamic_backend_maps, state->renewal.route_id,
                &state->registration_tls_endpoint,
                &state->registration_tls_map,
                state->registration_ca_bundle_id,
                state->registration_certificate_sha256,
                expires_at_ms, now_ms);
        }
        /* Renew the existing full endpoint snapshot. A renewal's TLS
         * identity descriptor from an older SDK has no path and must never
         * replace the path-bound registration lease. New SDKs resend the
         * immutable endpoint, allowing a restarted Gateway to rebuild the
         * in-memory TLS mapping without changing agentd route ownership. */
        (void)agent_invoke_dynamic_map_renew(
            &dynamic_backend_maps, state->renewal.route_id,
            expires_at_ms, now_ms);
    }
    return true;
}

static void send_lease_response(struct gateway_request_state *state)
{
    struct json_object *root = json_object_new_object();

    if (root == NULL) {
        send_error(state->connection, 500, "OUT_OF_MEMORY",
                   "failed to build lease response");
        return;
    }
    json_object_object_add(root, "route_id",
        json_object_new_string(state->lease_response.route_id));
    json_object_object_add(root, "generation",
        json_object_new_uint64(state->lease_response.generation));
    json_object_object_add(root, "lease_seconds",
        json_object_new_uint64(state->lease_response.lease_seconds));
    json_object_object_add(root, "removed",
        json_object_new_boolean(state->lease_response.removed));
    json_object_object_add(root, "backend_tls_automatic",
        json_object_new_boolean(state->registration_backend_tls));
    if (state->lease_response.public_ipv6[0] != '\0') {
        struct json_object *endpoint;

        json_object_object_add(root, "public_ipv6",
            json_object_new_string(state->lease_response.public_ipv6));
        if (config.public_descriptor_enabled) {
            json_object_object_add(root, "public_port",
                json_object_new_uint64(config.public_ingress_port));
            json_object_object_add(root, "public_scheme",
                json_object_new_string(config.public_scheme));
            if (strcmp(config.public_scheme, "https") == 0) {
                json_object_object_add(root, "tls_server_name",
                    json_object_new_string(config.public_tls_server_name));
                json_object_object_add(root, "ca_bundle_id",
                    json_object_new_string(config.public_ca_bundle_id));
            }
            endpoint = json_object_new_object();
            if (endpoint == NULL) {
                json_object_put(root);
                send_error(state->connection, 500, "OUT_OF_MEMORY",
                           "failed to build public endpoint descriptor");
                return;
            }
            json_object_object_add(endpoint, "scheme",
                json_object_new_string(config.public_scheme));
            json_object_object_add(endpoint, "address",
                json_object_new_string(state->lease_response.public_ipv6));
            json_object_object_add(endpoint, "port",
                json_object_new_uint64(config.public_ingress_port));
            if (strcmp(config.public_scheme, "https") == 0) {
                json_object_object_add(endpoint, "tls_server_name",
                    json_object_new_string(config.public_tls_server_name));
                json_object_object_add(endpoint, "ca_bundle_id",
                    json_object_new_string(config.public_ca_bundle_id));
            }
            json_object_object_add(root, "public_endpoint", endpoint);
        }
    }
    json_object_object_add(root, "tenant_claim_overridden",
        json_object_new_boolean(state->tenant_overridden));
    send_json(state->connection,
              state->operation == GATEWAY_OPERATION_REGISTER ? 201 : 200,
              root);
    json_object_put(root);
}

static void send_route_response(struct gateway_request_state *state)
{
    struct json_object *root = json_object_new_object();
    struct json_object *route = json_object_new_object();
    struct json_object *security = json_object_new_object();

    if (root == NULL || route == NULL || security == NULL) {
        if (root != NULL) {
            json_object_put(root);
        }
        if (route != NULL) {
            json_object_put(route);
        }
        if (security != NULL) {
            json_object_put(security);
        }
        send_error(state->connection, 500, "OUT_OF_MEMORY",
                   "failed to build route response");
        return;
    }

    json_object_object_add(root, "version", json_object_new_string("1.0"));
    json_object_object_add(root, "task_id",
                           json_object_new_string(state->task_id));
    json_object_object_add(root, "found",
                           json_object_new_boolean(state->response.found));
    json_object_object_add(
        root, "generation",
        json_object_new_uint64(state->response.generation));
    if (state->response.found) {
        json_object_object_add(
            route, "route_id",
            json_object_new_string(state->response.route_id));
        json_object_object_add(
            route, "origin",
            json_object_new_string(state->response.origin));
        json_object_object_add(
            route, "endpoint",
            json_object_new_string(state->response.endpoint));
        json_object_object_add(
            route, "source",
            json_object_new_string(state->response.source));
        json_object_object_add(
            route, "relay",
            json_object_new_boolean(state->response.relay));
        if (state->response.relay) {
            json_object_object_add(
                route, "relay_peer_id",
                json_object_new_string(state->response.relay_peer_id));
            json_object_object_add(
                route, "target_router_id",
                json_object_new_string(state->response.target_router_id));
        }
        json_object_object_add(
            route, "score",
            json_object_new_uint64(state->response.score));
        json_object_object_add(root, "route", route);
    } else {
        json_object_put(route);
    }
    json_object_object_add(security, "transport",
                           json_object_new_string("loopback-cleartext"));
    json_object_object_add(
        security, "identity_binding",
        json_object_new_boolean(state->identity.verified));
    json_object_object_add(
        security, "tenant_claim_overridden",
        json_object_new_boolean(state->tenant_overridden));
    json_object_object_add(root, "security", security);
    send_json(state->connection, 200, root);
    json_object_put(root);
}

static void fail_async_request(
    struct gateway_request_state *state,
    const char *message
)
{
    runtime.ipc_failures++;
    if (state->relay_ipc) runtime.invoke_failures++;
    (void)gateway_ipc_machine_transition(
        &state->ipc_machine, GATEWAY_IPC_IO_FAILED);
    if (state->relay_ipc && state->stream) runtime.stream_failures++;
    if (state->relay_ipc && state->stream && state->stream_head_sent &&
        !state->connection->closed(state->connection)) {
        state->connection->userdata = NULL;
        state->connection->close(state->connection);
    } else if (!state->connection->closed(state->connection)) {
        send_error(state->connection, 503,
                   "CONTROL_PLANE_UNAVAILABLE", message);
    }
    destroy_request_state(state);
}

static void set_ipc_events(
    struct gateway_request_state *state,
    int events
)
{
    struct ev_loop *loop = state->connection->get_loop(state->connection);

    if (ev_is_active(&state->ipc_watcher)) {
        ev_io_stop(loop, &state->ipc_watcher);
    }
    ev_io_set(&state->ipc_watcher, state->ipc_fd, events);
    ev_io_start(loop, &state->ipc_watcher);
}

static bool try_send_ipc_request(struct gateway_request_state *state)
{
    ssize_t sent;

    sent = send(state->ipc_fd, state->frame, state->frame_length,
                MSG_NOSIGNAL);
    if (sent == (ssize_t)state->frame_length) {
        if (!gateway_ipc_machine_transition(
                &state->ipc_machine, GATEWAY_IPC_REQUEST_SENT)) {
            return false;
        }
        set_ipc_events(state, EV_READ);
        return true;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        set_ipc_events(state, EV_WRITE);
        return true;
    }
    return false;
}

static bool decode_ipc_response(
    struct gateway_request_state *state,
    size_t received,
    char *error,
    size_t error_capacity
)
{
    struct agent_ipc_header header;
    struct agent_ipc_error_response ipc_error;
    const struct agent_ipc_route_candidate *candidate;

    if (agent_ipc_decode_header(state->frame, received,
                                &header) != AGENT_IPC_OK) {
        snprintf(error, error_capacity, "invalid IPC response");
        return false;
    }
    if (header.request_id != state->request.request_id) {
        snprintf(error, error_capacity, "IPC request ID mismatch");
        return false;
    }
    if (header.type == AGENT_IPC_ERROR_RESPONSE) {
        if (agent_ipc_decode_error_response(
                state->frame, received, &ipc_error) == AGENT_IPC_OK) {
            if (operation_is_registration(state->operation)) {
                state->ipc_application_error = true;
                state->ipc_application_error_code = ipc_error.code;
                (void)snprintf(state->ipc_application_error_message,
                               sizeof(state->ipc_application_error_message),
                               "%s", ipc_error.message);
                return true;
            }
            snprintf(error, error_capacity, "agentd: %s",
                     ipc_error.message);
        } else {
            snprintf(error, error_capacity, "agentd returned an error");
        }
        return false;
    }
    if (operation_is_registration(state->operation)) {
        if (header.type != AGENT_IPC_LEASE_RESPONSE ||
            agent_ipc_decode_lease_response(
                state->frame, received, &state->lease_response) !=
                AGENT_IPC_OK) {
            snprintf(error, error_capacity,
                     "invalid registration IPC response");
            return false;
        }
        return true;
    }
    if (state->relay_ipc) {
        if (header.type != AGENT_IPC_INVOKE_RESPONSE ||
            agent_ipc_decode_invoke_response(
                state->frame, received, &state->relay_response) !=
                AGENT_IPC_OK) {
            snprintf(error, error_capacity,
                     "invalid Relay invoke IPC response");
            return false;
        }
        return true;
    }
    if (state->candidate_lookup) {
        if (header.type != AGENT_IPC_CANDIDATES_RESPONSE ||
            agent_ipc_decode_candidates_response(
                state->frame, received, &state->candidates) !=
                AGENT_IPC_OK) {
            snprintf(error, error_capacity,
                     "invalid candidates lookup response");
            return false;
        }
        memset(&state->response, 0, sizeof(state->response));
        state->response.request_id = state->request.request_id;
        state->response.generation = state->candidates.generation;
        (void)copy_text(state->response.policy_id,
                        sizeof(state->response.policy_id),
                        state->candidates.policy_id);
        state->response.tenant_max_inflight =
            state->candidates.tenant_max_inflight;
        state->response.tenant_rate_per_second =
            state->candidates.tenant_rate_per_second;
        state->response.tenant_rate_burst =
            state->candidates.tenant_rate_burst;
        state->response.found = state->candidates.count > 0U;
        if (!state->response.found) return true;
        state->candidate_index = 0U;
        if (state->resume_requested) {
            size_t index;

            state->resume_route_available = false;
            for (index = 0U; index < state->candidates.count; index++) {
                if (strcmp(state->candidates.candidates[index].route_id,
                           state->resume_route_id) == 0) {
                    state->candidate_index = index;
                    state->resume_route_available = true;
                    break;
                }
            }
            if (!state->resume_route_available) {
                state->response.found = false;
                return true;
            }
        }
        candidate = &state->candidates.candidates[state->candidate_index];
        state->response.score = candidate->score;
        if (!copy_text(state->response.route_id,
                       sizeof(state->response.route_id),
                       candidate->route_id) ||
            !copy_text(state->response.origin,
                       sizeof(state->response.origin),
                       candidate->origin) ||
            !copy_text(state->response.endpoint,
                       sizeof(state->response.endpoint),
                       candidate->endpoint) ||
            !copy_text(state->response.source,
                       sizeof(state->response.source),
                       candidate->source) ||
            (candidate->relay &&
             (!copy_text(state->response.relay_peer_id,
                         sizeof(state->response.relay_peer_id),
                         candidate->relay_peer_id) ||
              !copy_text(state->response.target_router_id,
                         sizeof(state->response.target_router_id),
                         candidate->target_router_id)))) {
            snprintf(error, error_capacity,
                     "candidate fields exceed gateway bounds");
            return false;
        }
        state->response.relay = candidate->relay;
        return true;
    }
    if (agent_ipc_decode_lookup_response(
            state->frame, received, &state->response) != AGENT_IPC_OK) {
        snprintf(error, error_capacity, "invalid lookup response");
        return false;
    }
    return true;
}

static void close_ipc_transport(struct gateway_request_state *state)
{
    struct ev_loop *loop = state->connection->get_loop(state->connection);

    if (ev_is_active(&state->ipc_watcher)) {
        ev_io_stop(loop, &state->ipc_watcher);
    }
    if (ev_is_active(&state->deadline_watcher)) {
        ev_timer_stop(loop, &state->deadline_watcher);
    }
    if (state->ipc_fd >= 0) {
        close(state->ipc_fd);
        state->ipc_fd = -1;
    }
}

static void close_backend_attempt(struct gateway_request_state *state)
{
    struct ev_loop *loop;

    if (state == NULL) return;
    loop = state->connection != NULL
        ? state->connection->get_loop(state->connection) : NULL;
    if (loop != NULL) {
        if (ev_is_active(&state->backend_watcher)) {
            ev_io_stop(loop, &state->backend_watcher);
        }
        if (ev_is_active(&state->backend_deadline_watcher)) {
            ev_timer_stop(loop, &state->backend_deadline_watcher);
        }
    }
    agent_tls_client_connection_free(&state->tls_connection);
    if (state->backend_fd >= 0) close(state->backend_fd);
    state->backend_fd = -1;
    free(state->backend_request);
    free(state->backend_response);
    free(state->stream_event_buffer);
    state->backend_request = NULL;
    state->backend_response = NULL;
    state->stream_event_buffer = NULL;
    state->backend_request_length = 0U;
    state->backend_request_sent = 0U;
    state->backend_response_length = 0U;
    state->backend_response_capacity = 0U;
    state->backend_started = false;
    state->backend_tls = false;
    state->backend_tcp_connected = false;
    state->backend_certificate_sha256[0] = '\0';
    memset(&state->backend_endpoint, 0, sizeof(state->backend_endpoint));
    memset(&state->backend_machine, 0, sizeof(state->backend_machine));
}

static void set_backend_events(
    struct gateway_request_state *state,
    int events
)
{
    struct ev_loop *loop = state->connection->get_loop(state->connection);

    if (ev_is_active(&state->backend_watcher)) {
        ev_io_stop(loop, &state->backend_watcher);
    }
    ev_io_set(&state->backend_watcher, state->backend_fd, events);
    ev_io_start(loop, &state->backend_watcher);
}

static const struct agent_invoke_remote_map *find_remote_map(
    const struct agent_invoke_endpoint *endpoint,
    const char *route_id,
    const struct agent_invoke_dynamic_map **dynamic
)
{
    size_t index;
    const struct agent_invoke_dynamic_map *leased;

    if (dynamic != NULL) *dynamic = NULL;
    leased = agent_invoke_dynamic_map_find(
        &dynamic_backend_maps, route_id, endpoint, monotonic_ms());
    if (leased != NULL) {
        if (dynamic != NULL) *dynamic = leased;
        return &leased->mapping;
    }

    for (index = 0U; index < config.remote_map_count; index++) {
        if (agent_invoke_remote_map_matches(
                endpoint, &config.remote_maps[index])) {
            return &config.remote_maps[index];
        }
    }
    return NULL;
}

static enum agent_tls_io_result backend_read(
    struct gateway_request_state *state,
    char *buffer,
    size_t capacity,
    size_t *received
)
{
    ssize_t result;

    *received = 0U;
    if (state->backend_tls) {
        return agent_tls_client_read(
            &state->tls_connection, (unsigned char *)buffer,
            capacity, received);
    }
    result = recv(state->backend_fd, buffer, capacity, 0);
    if (result > 0) {
        *received = (size_t)result;
        return AGENT_TLS_IO_OK;
    }
    if (result == 0) return AGENT_TLS_IO_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return AGENT_TLS_IO_WANT_READ;
    }
    return AGENT_TLS_IO_ERROR;
}

static bool advance_tls_handshake(struct gateway_request_state *state);
static bool start_async_backend(
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
);
static bool start_async_relay_ipc(
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
);

static void send_relay_response(struct gateway_request_state *state)
{
    struct agent_ipc_invoke_response *response = &state->relay_response;

    state->connection->send_head(
        state->connection, response->status_code,
        (int64_t)response->body_length, NULL);
    state->connection->send_header(
        state->connection, "Content-Type", "%s", response->content_type);
    state->connection->send_header(
        state->connection, "Cache-Control", "no-store");
    state->connection->send_header(
        state->connection, "X-Content-Type-Options", "nosniff");
    state->connection->send_header(
        state->connection, "X-Nexus-Route-Id", "%s",
        state->response.route_id);
    state->connection->send_header(
        state->connection, "X-Nexus-Transport", "relay");
    state->connection->end_headers(state->connection);
    if (response->body_length > 0U) {
        state->connection->send(
            state->connection, (const char *)response->body,
            response->body_length);
    }
    state->connection->end_response(state->connection);
}

enum relay_stream_ipc_result {
    RELAY_STREAM_IPC_MORE = 0,
    RELAY_STREAM_IPC_DONE,
    RELAY_STREAM_IPC_ERROR
};

static void reset_relay_ipc_timer(struct gateway_request_state *state)
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

static enum relay_stream_ipc_result handle_relay_stream_ipc(
    struct gateway_request_state *state,
    size_t received,
    char *error,
    size_t error_capacity
)
{
    struct agent_ipc_header header;

    if (agent_ipc_decode_header(state->frame, received, &header) !=
            AGENT_IPC_OK ||
        header.request_id != state->request.request_id) {
        snprintf(error, error_capacity, "invalid Relay stream IPC frame");
        return RELAY_STREAM_IPC_ERROR;
    }
    if (header.type == AGENT_IPC_ERROR_RESPONSE) {
        struct agent_ipc_error_response response;

        if (agent_ipc_decode_error_response(
                state->frame, received, &response) == AGENT_IPC_OK) {
            snprintf(error, error_capacity, "agentd: %s", response.message);
        } else {
            snprintf(error, error_capacity, "agentd Relay stream failed");
        }
        return RELAY_STREAM_IPC_ERROR;
    }
    if (header.type == AGENT_IPC_STREAM_START) {
        struct agent_ipc_stream_start start;

        if (state->stream_head_sent ||
            agent_ipc_decode_stream_start(
                state->frame, received, &start) != AGENT_IPC_OK ||
            strcmp(start.content_type, "text/event-stream") != 0) {
            snprintf(error, error_capacity,
                     "invalid Relay SSE response start");
            return RELAY_STREAM_IPC_ERROR;
        }
        state->connection->send_head(state->connection, 200, -1, NULL);
        state->connection->send_header(
            state->connection, "Content-Type", "text/event-stream");
        state->connection->send_header(
            state->connection, "Cache-Control", "no-store");
        state->connection->send_header(
            state->connection, "X-Accel-Buffering", "no");
        state->connection->send_header(
            state->connection, "X-Nexus-Route-Id", "%s",
            state->response.route_id);
        state->connection->send_header(
            state->connection, "X-Nexus-Transport", "relay");
        state->connection->end_headers(state->connection);
        state->stream_head_sent = true;
        runtime.streams_started++;
        runtime.relay_streams_started++;
        reset_relay_ipc_timer(state);
        return RELAY_STREAM_IPC_MORE;
    }
    if (header.type == AGENT_IPC_STREAM_DATA) {
        struct agent_ipc_stream_data data;

        if (!state->stream_head_sent ||
            agent_ipc_decode_stream_data(
                state->frame, received, &data) != AGENT_IPC_OK ||
            state->relay_stream_bytes > config.max_backend_response_bytes ||
            data.data_length > config.max_backend_response_bytes -
                                   state->relay_stream_bytes ||
            state->connection->closed(state->connection)) {
            snprintf(error, error_capacity,
                     "Relay SSE data exceeded its bound");
            return RELAY_STREAM_IPC_ERROR;
        }
        state->connection->send(
            state->connection, (const char *)data.data, data.data_length);
        state->relay_stream_bytes += data.data_length;
        runtime.stream_bytes += data.data_length;
        runtime.relay_stream_frames++;
        reset_relay_ipc_timer(state);
        return RELAY_STREAM_IPC_MORE;
    }
    if (header.type == AGENT_IPC_STREAM_END) {
        struct agent_ipc_stream_end end;

        if (!state->stream_head_sent ||
            agent_ipc_decode_stream_end(
                state->frame, received, &end) != AGENT_IPC_OK ||
            end.total_bytes != state->relay_stream_bytes) {
            snprintf(error, error_capacity, "invalid Relay SSE completion");
            return RELAY_STREAM_IPC_ERROR;
        }
        if (!state->connection->closed(state->connection)) {
            state->connection->end_response(state->connection);
        }
        runtime.invoke_completed++;
        runtime.streams_completed++;
        runtime.relay_streams_completed++;
        return RELAY_STREAM_IPC_DONE;
    }
    snprintf(error, error_capacity, "unexpected Relay stream IPC type");
    return RELAY_STREAM_IPC_ERROR;
}

static bool select_route_candidate(
    struct gateway_request_state *state,
    size_t index
)
{
    const struct agent_ipc_route_candidate *candidate;

    if (index >= state->candidates.count) return false;
    candidate = &state->candidates.candidates[index];
    memset(&state->response, 0, sizeof(state->response));
    state->response.request_id = state->request.request_id;
    state->response.found = true;
    state->response.generation = state->candidates.generation;
    (void)copy_text(state->response.policy_id,
                    sizeof(state->response.policy_id),
                    state->candidates.policy_id);
    state->response.tenant_max_inflight =
        state->candidates.tenant_max_inflight;
    state->response.tenant_rate_per_second =
        state->candidates.tenant_rate_per_second;
    state->response.tenant_rate_burst = state->candidates.tenant_rate_burst;
    state->response.score = candidate->score;
    return copy_text(state->response.route_id,
                     sizeof(state->response.route_id),
                     candidate->route_id) &&
           copy_text(state->response.origin,
                     sizeof(state->response.origin),
                     candidate->origin) &&
           copy_text(state->response.endpoint,
                     sizeof(state->response.endpoint),
                     candidate->endpoint) &&
           copy_text(state->response.source,
                     sizeof(state->response.source),
                     candidate->source) &&
           (!candidate->relay ||
            (copy_text(state->response.relay_peer_id,
                       sizeof(state->response.relay_peer_id),
                       candidate->relay_peer_id) &&
             copy_text(state->response.target_router_id,
                       sizeof(state->response.target_router_id),
                       candidate->target_router_id))) &&
           ((state->response.relay = candidate->relay), true);
}

static bool try_next_route_candidate(struct gateway_request_state *state)
{
    char error[256];
    uint64_t now;
    uint64_t remaining;

    if (!state->retry_eligible || state->stream_head_sent ||
        state->backend_response_length > 0U) return false;
    while (state->candidate_index + 1U < state->candidates.count) {
        now = monotonic_ms();
        if (now >= state->backend_deadline_ms) {
            runtime.retry_budget_rejections++;
            return false;
        }
        remaining = state->backend_deadline_ms - now;
        if (remaining < config.retry_min_remaining_ms) {
            runtime.retry_budget_rejections++;
            return false;
        }
        close_backend_attempt(state);
        state->candidate_index++;
        if (!select_route_candidate(state, state->candidate_index)) {
            return false;
        }
        runtime.retry_attempts++;
        error[0] = '\0';
        if (start_async_backend(state, error, sizeof(error))) return true;
    }
    return false;
}

static void fail_backend_request(
    struct gateway_request_state *state,
    const char *code,
    const char *message
)
{
    (void)agent_invoke_backend_machine_transition(
        &state->backend_machine, AGENT_BACKEND_IO_FAILED);
    if (try_next_route_candidate(state)) return;
    if (state->retry_eligible && state->candidates.count > 0U) {
        runtime.retry_exhausted++;
    }
    if (state->internal_invoke) runtime.internal_invoke_failures++;
    runtime.invoke_failures++;
    if (state->stream) runtime.stream_failures++;
    if (state->stream_head_sent &&
        !state->connection->closed(state->connection)) {
        state->connection->userdata = NULL;
        state->connection->close(state->connection);
    } else if (!state->connection->closed(state->connection)) {
        send_error(state->connection, 502, code, message);
    }
    destroy_request_state(state);
}

static void send_backend_response(
    struct gateway_request_state *state,
    const struct agent_invoke_http_response *response
)
{
    const char *body = state->backend_response + response->header_length;

    if (state->internal_invoke) runtime.internal_invoke_completed++;

    state->connection->send_head(
        state->connection, response->status,
        (int64_t)response->body_length, NULL);
    state->connection->send_header(
        state->connection, "Content-Type", "%s", response->content_type);
    state->connection->send_header(
        state->connection, "Cache-Control", "no-store");
    state->connection->send_header(
        state->connection, "X-Content-Type-Options", "nosniff");
    state->connection->send_header(
        state->connection, "X-Nexus-Route-Id", "%s",
        state->response.route_id);
    state->connection->end_headers(state->connection);
    if (response->body_length > 0U) {
        state->connection->send(
            state->connection, body, response->body_length);
    }
    state->connection->end_response(state->connection);
}

static bool try_send_backend_request(struct gateway_request_state *state)
{
    ssize_t sent;
    size_t tls_sent = 0U;
    size_t remaining =
        state->backend_request_length - state->backend_request_sent;

    if (state->backend_tls) {
        enum agent_tls_io_result tls_result = agent_tls_client_write(
            &state->tls_connection,
            (const unsigned char *)state->backend_request +
                state->backend_request_sent,
            remaining, &tls_sent);

        if (tls_result == AGENT_TLS_IO_WANT_READ) {
            set_backend_events(state, EV_READ);
            return true;
        }
        if (tls_result == AGENT_TLS_IO_WANT_WRITE) {
            set_backend_events(state, EV_WRITE);
            return true;
        }
        if (tls_result != AGENT_TLS_IO_OK) return false;
        sent = (ssize_t)tls_sent;
    } else {
        sent = send(
            state->backend_fd,
            state->backend_request + state->backend_request_sent,
            remaining,
            MSG_NOSIGNAL);
    }
    if (sent > 0) {
        state->backend_request_sent += (size_t)sent;
        if (state->backend_request_sent == state->backend_request_length) {
            if (!agent_invoke_backend_machine_transition(
                    &state->backend_machine,
                    AGENT_BACKEND_REQUEST_SENT)) {
                return false;
            }
            set_backend_events(state, EV_READ);
        }
        if (state->backend_request_sent < state->backend_request_length) {
            set_backend_events(state, EV_WRITE);
        }
        return true;
    }
    return sent < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
}

static bool advance_tls_handshake(struct gateway_request_state *state)
{
    enum agent_tls_io_result result = agent_tls_client_handshake(
        &state->tls_connection);

    if (result == AGENT_TLS_IO_WANT_READ) {
        set_backend_events(state, EV_READ);
        return true;
    }
    if (result == AGENT_TLS_IO_WANT_WRITE) {
        set_backend_events(state, EV_WRITE);
        return true;
    }
    if (result != AGENT_TLS_IO_OK) {
        runtime.remote_tls_failures++;
        return false;
    }
    if (!agent_tls_client_peer_sha256_matches(
            &state->tls_connection,
            state->backend_certificate_sha256)) {
        runtime.remote_tls_failures++;
        return false;
    }
    runtime.remote_tls_handshakes++;
    if (!agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_CONNECTED)) return false;
    return try_send_backend_request(state);
}

static void reset_stream_idle_timer(struct gateway_request_state *state)
{
    struct ev_loop *loop = state->connection->get_loop(state->connection);
    ev_tstamp timeout =
        (ev_tstamp)config.stream_idle_timeout_ms / 1000.0;

    if (ev_is_active(&state->backend_deadline_watcher)) {
        ev_timer_stop(loop, &state->backend_deadline_watcher);
    }
    ev_timer_set(&state->backend_deadline_watcher, timeout, 0.0);
    ev_timer_start(loop, &state->backend_deadline_watcher);
}

static bool emit_stream_event(
    const char *event,
    size_t event_length,
    void *context
)
{
    struct gateway_request_state *state = context;

    if (state->connection->closed(state->connection) ||
        event_length > config.max_backend_response_bytes -
            (size_t)state->stream_relay.emitted_bytes) {
        return false;
    }
    state->connection->send(state->connection, event, event_length);
    runtime.stream_events++;
    runtime.stream_bytes += event_length;
    return true;
}

static void complete_stream(struct gateway_request_state *state)
{
    (void)agent_invoke_backend_machine_transition(
        &state->backend_machine, AGENT_BACKEND_RESPONSE_RECEIVED);
    runtime.invoke_completed++;
    if (state->retry_eligible && state->candidate_index > 0U) {
        runtime.retry_successes++;
    }
    runtime.streams_completed++;
    if (!state->connection->closed(state->connection)) {
        state->connection->end_response(state->connection);
    }
    destroy_request_state(state);
}

static void handle_stream_read(struct gateway_request_state *state)
{
    struct agent_sse_response_head head;
    enum agent_sse_head_result head_result;
    enum agent_sse_relay_result relay_result;
    char *destination;
    size_t capacity;
    size_t received;
    enum agent_tls_io_result io_result;

    if (state->stream_head_parsed) {
        destination = state->backend_response;
        capacity = state->backend_response_capacity - 1U;
    } else {
        if (state->backend_response_length + 1U >=
            state->backend_response_capacity) {
            fail_backend_request(state, "INVALID_STREAM_RESPONSE",
                                 "stream response headers exceed the bound");
            return;
        }
        destination = state->backend_response + state->backend_response_length;
        capacity = state->backend_response_capacity -
            state->backend_response_length - 1U;
    }
    io_result = backend_read(
        state, destination, capacity, &received);
    if (io_result == AGENT_TLS_IO_WANT_READ) {
        set_backend_events(state, EV_READ);
        return;
    }
    if (io_result == AGENT_TLS_IO_WANT_WRITE) {
        set_backend_events(state, EV_WRITE);
        return;
    }
    if (io_result == AGENT_TLS_IO_ERROR) {
        fail_backend_request(state, "BACKEND_UNAVAILABLE",
                             "stream backend read failed");
        return;
    }
    if (received > 0U) {
        set_backend_events(state, EV_READ);
        reset_stream_idle_timer(state);
    }
    if (!state->stream_head_parsed) {
        state->backend_response_length += received;
        state->backend_response[state->backend_response_length] = '\0';
        head_result = agent_sse_parse_response_head(
            state->backend_response, state->backend_response_length, &head);
        if (head_result == AGENT_SSE_HEAD_INCOMPLETE && received > 0U) return;
        if (head_result != AGENT_SSE_HEAD_OK ||
            !agent_sse_response_is_streamable(&head)) {
            fail_backend_request(state, "INVALID_STREAM_RESPONSE",
                                 "backend did not return bounded SSE");
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
        state->connection->send_header(
            state->connection, "X-Nexus-Route-Id", "%s",
            state->response.route_id);
        state->connection->end_headers(state->connection);
        state->stream_head_sent = true;
        runtime.streams_started++;
        destination = state->backend_response + head.header_length;
        capacity = state->backend_response_length - head.header_length;
    } else {
        capacity = received;
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
            fail_backend_request(
                state,
                relay_result == AGENT_SSE_RELAY_TOO_LARGE
                    ? "STREAM_EVENT_TOO_LARGE" : "INVALID_STREAM_RESPONSE",
                "stream event framing violated the configured contract");
            return;
        }
    }
    state->backend_response_length = 0U;
    if (io_result == AGENT_TLS_IO_CLOSED) {
        relay_result = agent_sse_relay_finish(&state->stream_relay);
        if (relay_result == AGENT_SSE_RELAY_DONE) complete_stream(state);
        else fail_backend_request(state, "INVALID_STREAM_RESPONSE",
                                  "stream ended inside an event");
    }
}

static void backend_event_callback(
    struct ev_loop *loop,
    ev_io *watcher,
    int revents
)
{
    struct gateway_request_state *state = watcher->data;
    struct agent_invoke_http_response response;
    enum agent_invoke_http_result parse_result;
    enum agent_tls_io_result io_result;
    socklen_t socket_error_length;
    size_t received;
    int socket_error = 0;

    (void)loop;
    if ((revents & EV_ERROR) != 0) {
        fail_backend_request(state, "BACKEND_UNAVAILABLE",
                             "backend watcher failed");
        return;
    }
    if (state->backend_machine.phase == AGENT_BACKEND_CONNECTING) {
        if (!state->backend_tcp_connected) {
            if ((revents & EV_WRITE) == 0) {
                fail_backend_request(state, "BACKEND_UNAVAILABLE",
                                     "unexpected backend connect event");
                return;
            }
            socket_error_length = sizeof(socket_error);
            if (getsockopt(state->backend_fd, SOL_SOCKET, SO_ERROR,
                           &socket_error, &socket_error_length) != 0 ||
                socket_error != 0) {
                fail_backend_request(state, "BACKEND_UNAVAILABLE",
                                     "backend connection failed");
                return;
            }
            state->backend_tcp_connected = true;
        }
        if (state->backend_tls) {
            if (!advance_tls_handshake(state)) {
                fail_backend_request(state, "BACKEND_TLS_FAILED",
                                     "remote backend TLS verification failed");
            }
        } else if (!agent_invoke_backend_machine_transition(
                       &state->backend_machine, AGENT_BACKEND_CONNECTED) ||
                   !try_send_backend_request(state)) {
            fail_backend_request(state, "BACKEND_UNAVAILABLE",
                                 "backend connection failed");
        }
        return;
    }
    if (state->backend_machine.phase == AGENT_BACKEND_SENDING) {
        if (!try_send_backend_request(state)) {
            fail_backend_request(state, "BACKEND_UNAVAILABLE",
                                 "backend write failed");
        }
        return;
    }
    if (state->backend_machine.phase != AGENT_BACKEND_RECEIVING) {
        fail_backend_request(state, "BACKEND_UNAVAILABLE",
                             "unexpected backend event");
        return;
    }
    if (state->stream) {
        handle_stream_read(state);
        return;
    }
    if (state->backend_response_length + 1U >=
        state->backend_response_capacity) {
        fail_backend_request(state, "INVALID_BACKEND_RESPONSE",
                             "backend response exceeds configured bound");
        return;
    }
    io_result = backend_read(
        state,
        state->backend_response + state->backend_response_length,
        state->backend_response_capacity -
            state->backend_response_length - 1U,
        &received);
    if (io_result == AGENT_TLS_IO_WANT_READ) {
        set_backend_events(state, EV_READ);
        return;
    }
    if (io_result == AGENT_TLS_IO_WANT_WRITE) {
        set_backend_events(state, EV_WRITE);
        return;
    }
    if (io_result == AGENT_TLS_IO_ERROR) {
        fail_backend_request(state, "BACKEND_UNAVAILABLE",
                             "backend read failed");
        return;
    }
    if (received > 0U) {
        set_backend_events(state, EV_READ);
        state->backend_response_length += received;
        state->backend_response[state->backend_response_length] = '\0';
    }
    parse_result = agent_invoke_parse_http_response(
        state->backend_response,
        state->backend_response_length,
        state->effective_max_backend_response_bytes,
        &response);
    if (parse_result == AGENT_INVOKE_HTTP_INCOMPLETE && received > 0U) {
        return;
    }
    if (parse_result != AGENT_INVOKE_HTTP_OK) {
        if (parse_result == AGENT_INVOKE_HTTP_TOO_LARGE) {
            fail_backend_request(state, "INVALID_BACKEND_RESPONSE",
                                 "backend response exceeds configured bound");
        } else if (parse_result == AGENT_INVOKE_HTTP_REDIRECT) {
            fail_backend_request(state, "BACKEND_REDIRECT_REJECTED",
                                 "backend redirects are not allowed");
        } else {
            fail_backend_request(state, "INVALID_BACKEND_RESPONSE",
                                 "backend returned an invalid HTTP response");
        }
        return;
    }
    if (!agent_invoke_backend_machine_transition(
            &state->backend_machine, AGENT_BACKEND_RESPONSE_RECEIVED)) {
        fail_backend_request(state, "BACKEND_UNAVAILABLE",
                             "invalid backend lifecycle");
        return;
    }
    runtime.invoke_completed++;
    if (state->retry_eligible && state->candidate_index > 0U) {
        runtime.retry_successes++;
    }
    if (!state->connection->closed(state->connection)) {
        send_backend_response(state, &response);
    }
    destroy_request_state(state);
}

static void backend_deadline_callback(
    struct ev_loop *loop,
    ev_timer *watcher,
    int revents
)
{
    struct gateway_request_state *state = watcher->data;

    (void)loop;
    (void)revents;
    runtime.backend_timeouts++;
    (void)agent_invoke_backend_machine_transition(
        &state->backend_machine, AGENT_BACKEND_DEADLINE_EXPIRED);
    if (try_next_route_candidate(state)) return;
    if (state->retry_eligible && state->candidates.count > 0U) {
        runtime.retry_exhausted++;
    }
    if (state->internal_invoke) runtime.internal_invoke_failures++;
    runtime.invoke_failures++;
    if (state->stream) runtime.stream_failures++;
    if (state->stream_head_sent &&
        !state->connection->closed(state->connection)) {
        state->connection->userdata = NULL;
        state->connection->close(state->connection);
    } else if (!state->connection->closed(state->connection)) {
        send_error(state->connection, 504, "BACKEND_TIMEOUT",
                   "backend deadline expired");
    }
    destroy_request_state(state);
}

static bool start_async_backend(
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
)
{
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
    struct sockaddr *address;
    socklen_t address_length;
    const struct agent_invoke_remote_map *remote_map = NULL;
    const struct agent_invoke_dynamic_map *dynamic_map = NULL;
    const char *connect_host;
    bool connect_ipv6 = false;
    struct ev_loop *loop = state->connection->get_loop(state->connection);
    size_t request_capacity;
    ev_tstamp timeout;
    uint64_t now;
    uint64_t remaining;
    int connect_result;

    now = monotonic_ms();
    if (now >= state->backend_deadline_ms) {
        snprintf(error, error_capacity, "backend deadline exhausted");
        return false;
    }
    remaining = state->backend_deadline_ms - now;
    if (remaining > UINT32_MAX) remaining = UINT32_MAX;
    state->effective_backend_timeout_ms = (uint32_t)remaining;
    if (state->retry_eligible &&
        state->effective_backend_timeout_ms >
            config.retry_attempt_timeout_ms) {
        state->effective_backend_timeout_ms =
            config.retry_attempt_timeout_ms;
    }

    if (agent_invoke_parse_loopback_endpoint(
            state->response.endpoint, &state->backend_endpoint)) {
        connect_host = state->backend_endpoint.host;
    } else if (config.lan_backend_enabled &&
               agent_invoke_parse_lan_endpoint(
                   state->response.endpoint, &state->backend_endpoint)) {
        connect_host = state->backend_endpoint.host;
    } else if (config.remote_backend_enabled &&
               agent_invoke_parse_remote_tls_endpoint(
                   state->response.endpoint, &state->backend_endpoint)) {
        if (state->internal_invoke) {
            if (!state->internal_dynamic_map_pinned ||
                !agent_invoke_dynamic_map_matches(
                    &state->internal_dynamic_map,
                    state->response.route_id,
                    &state->backend_endpoint, now)) {
                runtime.backend_endpoint_rejections++;
                snprintf(error, error_capacity,
                         "internal HTTPS lease expired or changed");
                return false;
            }
            dynamic_map = &state->internal_dynamic_map;
            remote_map = &dynamic_map->mapping;
        } else {
            remote_map = find_remote_map(
                &state->backend_endpoint, state->response.route_id,
                &dynamic_map);
        }
        if (remote_map == NULL) {
            runtime.backend_endpoint_rejections++;
            snprintf(error, error_capacity,
                     "remote HTTPS route has no trusted mapping");
            return false;
        }
        state->backend_tls = true;
        connect_host = remote_map->address;
        connect_ipv6 = remote_map->family == AGENT_INVOKE_IPV6;
        if (dynamic_map != NULL) {
            (void)snprintf(
                state->backend_certificate_sha256,
                sizeof(state->backend_certificate_sha256), "%s",
                dynamic_map->certificate_sha256);
        }
    } else {
        runtime.backend_endpoint_rejections++;
        snprintf(error, error_capacity,
                 "route endpoint is not loopback HTTP, an enabled private "
                 "LAN endpoint, or a trusted registered HTTPS identity");
        return false;
    }
    request_capacity = state->forward_body_length +
        GATEWAY_BACKEND_REQUEST_HEADER_RESERVE + 1U;
    if (request_capacity < state->forward_body_length) {
        snprintf(error, error_capacity, "backend request size overflow");
        return false;
    }
    state->backend_request = malloc(request_capacity);
    state->effective_max_backend_response_bytes =
        agent_invoke_backend_response_limit(
            state->internal_invoke,
            config.max_backend_response_bytes);
    state->backend_response_capacity = state->stream
        ? AGENT_SSE_MAX_HEADER_BYTES + 4097U
        : state->effective_max_backend_response_bytes +
          AGENT_INVOKE_MAX_HEADER_BYTES + 1U;
    state->backend_response = malloc(state->backend_response_capacity);
    if (state->stream) {
        state->stream_event_buffer = malloc(config.max_stream_event_bytes);
    }
    if (state->backend_request == NULL || state->backend_response == NULL ||
        (state->stream && state->stream_event_buffer == NULL) ||
        !(state->stream
            ? agent_invoke_build_stream_http_request(
                &state->backend_endpoint, state->response.route_id,
                state->forward_body, state->forward_body_length,
                state->backend_request, request_capacity,
                &state->backend_request_length)
            : agent_invoke_build_http_request(
                &state->backend_endpoint, state->response.route_id,
                state->forward_body, state->forward_body_length,
                state->backend_request, request_capacity,
                &state->backend_request_length))) {
        snprintf(error, error_capacity,
                 "failed to build bounded backend request");
        return false;
    }
    state->backend_response[0] = '\0';
    if (connect_ipv6 || (!state->backend_tls &&
        state->backend_endpoint.family == AGENT_INVOKE_IPV6)) {
        state->backend_fd = socket(
            AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        memset(&ipv6, 0, sizeof(ipv6));
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(state->backend_endpoint.port);
        if (inet_pton(AF_INET6, connect_host,
                      &ipv6.sin6_addr) != 1) {
            snprintf(error, error_capacity, "invalid IPv6 endpoint");
            return false;
        }
        address = (struct sockaddr *)&ipv6;
        address_length = sizeof(ipv6);
    } else {
        state->backend_fd = socket(
            AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        memset(&ipv4, 0, sizeof(ipv4));
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(state->backend_endpoint.port);
        if (inet_pton(AF_INET, connect_host,
                      &ipv4.sin_addr) != 1) {
            snprintf(error, error_capacity, "invalid IPv4 endpoint");
            return false;
        }
        address = (struct sockaddr *)&ipv4;
        address_length = sizeof(ipv4);
    }
    if (state->backend_fd < 0) {
        snprintf(error, error_capacity, "failed to create backend socket");
        return false;
    }
    if (state->backend_tls &&
        !agent_tls_client_connection_init(
            &tls_client, &state->tls_connection, state->backend_fd,
            state->backend_endpoint.host)) {
        runtime.remote_tls_failures++;
        snprintf(error, error_capacity,
                 "failed to initialize remote backend TLS");
        return false;
    }
    state->backend_started = true;
    agent_invoke_backend_machine_init(&state->backend_machine);
    ev_io_init(&state->backend_watcher, backend_event_callback,
               state->backend_fd, EV_WRITE);
    state->backend_watcher.data = state;
    timeout = (ev_tstamp)(state->stream
        ? config.stream_idle_timeout_ms
        : state->effective_backend_timeout_ms) / 1000.0;
    ev_timer_init(&state->backend_deadline_watcher,
                  backend_deadline_callback, timeout, 0.0);
    state->backend_deadline_watcher.data = state;
    ev_timer_start(loop, &state->backend_deadline_watcher);

    connect_result = connect(
        state->backend_fd, address, address_length);
    if (connect_result == 0) {
        state->backend_tcp_connected = true;
        if (state->backend_tls) {
            if (!advance_tls_handshake(state)) {
                snprintf(error, error_capacity,
                         "remote backend TLS handshake failed");
                return false;
            }
        } else if (!agent_invoke_backend_machine_transition(
                       &state->backend_machine, AGENT_BACKEND_CONNECTED) ||
                   !try_send_backend_request(state)) {
            snprintf(error, error_capacity, "backend write failed");
            return false;
        }
        return true;
    }
    if (errno != EINPROGRESS && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        snprintf(error, error_capacity, "backend connection failed");
        return false;
    }
    ev_io_start(loop, &state->backend_watcher);
    return true;
}

static bool admit_tenant_policy(struct gateway_request_state *state)
{
    enum agent_tenant_admission_result result;

    result = agent_tenant_quota_acquire(
        &runtime.tenant_quotas, state->request.tenant,
        state->response.tenant_max_inflight,
        state->response.tenant_rate_per_second,
        state->response.tenant_rate_burst, monotonic_ms());
    if (result == AGENT_TENANT_ADMITTED) {
        state->tenant_quota_counted =
            state->response.tenant_max_inflight != 0U ||
            state->response.tenant_rate_per_second != 0U;
        return true;
    }
    if (result == AGENT_TENANT_RATE_LIMITED) {
        runtime.tenant_rate_rejections++;
        if (!state->connection->closed(state->connection))
            send_error(state->connection, 429, "TENANT_RATE_LIMITED",
                       "Agent policy tenant rate exceeded");
    } else if (result == AGENT_TENANT_CONCURRENCY_LIMITED) {
        runtime.tenant_concurrency_rejections++;
        if (!state->connection->closed(state->connection))
            send_error(state->connection, 503, "TENANT_CONCURRENCY_LIMITED",
                       "Agent policy tenant concurrency exceeded");
    } else {
        runtime.tenant_quota_table_rejections++;
        if (!state->connection->closed(state->connection))
            send_error(state->connection, 503, "TENANT_ADMISSION_UNAVAILABLE",
                       "Agent policy tenant admission is unavailable");
    }
    return false;
}

static bool retain_stream_route(struct gateway_request_state *state)
{
    enum agent_stream_resume_result result;

    if (!state->stream || !config.stream_resume_enabled) return true;
    result = agent_stream_resume_cache_store(
        &stream_resume_cache, state->task_id, state->request.tenant,
        state->source_agent, state->request.intent, state->response.route_id,
        monotonic_ms());
    if (result == AGENT_STREAM_RESUME_OK) {
        if (!state->resume_requested) runtime.resume_routes_stored++;
        return true;
    }
    if (result == AGENT_STREAM_RESUME_CAPACITY) {
        runtime.resume_capacity_rejections++;
    }
    return false;
}

static void ipc_event_callback(
    struct ev_loop *loop,
    ev_io *watcher,
    int revents
)
{
    struct gateway_request_state *state = watcher->data;
    socklen_t socket_error_length;
    char error[256] = {0};
    ssize_t received;
    int socket_error = 0;

    (void)loop;
    if ((revents & EV_ERROR) != 0) {
        fail_async_request(state, "agentd IPC watcher failed");
        return;
    }

    if (state->ipc_machine.phase == GATEWAY_IPC_CONNECTING &&
        (revents & EV_WRITE) != 0) {
        socket_error_length = sizeof(socket_error);
        if (getsockopt(state->ipc_fd, SOL_SOCKET, SO_ERROR,
                       &socket_error, &socket_error_length) != 0 ||
            socket_error != 0 ||
            !gateway_ipc_machine_transition(
                &state->ipc_machine, GATEWAY_IPC_CONNECTED) ||
            !try_send_ipc_request(state)) {
            fail_async_request(state, "agentd IPC connection failed");
        }
        return;
    }

    if (state->ipc_machine.phase == GATEWAY_IPC_SENDING &&
        (revents & EV_WRITE) != 0) {
        if (!try_send_ipc_request(state)) {
            fail_async_request(state, "agentd IPC send failed");
        }
        return;
    }

    if (state->ipc_machine.phase != GATEWAY_IPC_RECEIVING ||
        (revents & EV_READ) == 0) {
        fail_async_request(state, "unexpected agentd IPC event");
        return;
    }

    received = recv(state->ipc_fd, state->frame,
                    sizeof(state->frame), MSG_TRUNC);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (received <= 0 || (size_t)received > sizeof(state->frame)) {
        fail_async_request(state, "agentd IPC receive failed");
        return;
    }
    if (state->relay_ipc && state->stream) {
        enum relay_stream_ipc_result stream_result =
            handle_relay_stream_ipc(
                state, (size_t)received, error, sizeof(error));

        if (stream_result == RELAY_STREAM_IPC_ERROR) {
            fail_async_request(
                state, error[0] != '\0' ? error :
                    "agentd Relay stream failed");
        } else if (stream_result == RELAY_STREAM_IPC_DONE) {
            (void)gateway_ipc_machine_transition(
                &state->ipc_machine, GATEWAY_IPC_RESPONSE_RECEIVED);
            runtime.ipc_completed++;
            destroy_request_state(state);
        }
        return;
    }
    if (!decode_ipc_response(state, (size_t)received,
                             error, sizeof(error))) {
        fail_async_request(
            state, error[0] != '\0' ? error : "agentd IPC receive failed");
        return;
    }
    if (!gateway_ipc_machine_transition(
            &state->ipc_machine, GATEWAY_IPC_RESPONSE_RECEIVED)) {
        fail_async_request(state, "invalid agentd IPC lifecycle");
        return;
    }
    runtime.ipc_completed++;
    if (operation_is_registration(state->operation)) {
        if (!state->connection->closed(state->connection)) {
            if (state->ipc_application_error) {
                uint32_t status = state->ipc_application_error_code;
                if (status < 400U || status > 599U) status = 500U;
                send_error(state->connection, (int)status,
                           "REGISTRATION_REJECTED",
                           state->ipc_application_error_message[0] != '\0' ?
                               state->ipc_application_error_message :
                               "agentd rejected the registration operation");
            } else if (!synchronize_registration_backend_map(state)) {
                send_error(state->connection, 503,
                           "BACKEND_IDENTITY_CAPACITY_EXHAUSTED",
                           "router HTTPS Agent identity table is full");
            } else {
                send_lease_response(state);
            }
        }
        destroy_request_state(state);
        return;
    }
    if (state->relay_ipc) {
        runtime.invoke_completed++;
        if (!state->connection->closed(state->connection)) {
            send_relay_response(state);
        }
        destroy_request_state(state);
        return;
    }
    if (!state->invoke) {
        if (!state->connection->closed(state->connection)) {
            send_route_response(state);
        }
        destroy_request_state(state);
        return;
    }

    close_ipc_transport(state);
    if (!state->response.found) {
        runtime.invoke_failures++;
        if (state->public_ingress) runtime.public_ingress_route_misses++;
        if (!state->connection->closed(state->connection)) {
            if (state->resume_requested) {
                runtime.resume_route_unavailable++;
                send_error(state->connection, 409,
                           "RESUME_ROUTE_UNAVAILABLE",
                           "the original task route is no longer eligible");
            } else {
                send_error(state->connection, 404, "NO_ROUTE",
                           "no eligible Agent route was found");
            }
        }
        destroy_request_state(state);
        return;
    }
    if (!admit_tenant_policy(state)) {
        runtime.invoke_failures++;
        destroy_request_state(state);
        return;
    }
    if (!retain_stream_route(state)) {
        runtime.invoke_failures++;
        if (!state->connection->closed(state->connection)) {
            send_error(state->connection, 503, "RESUME_CAPACITY_EXHAUSTED",
                       "router resumable-task route capacity is exhausted");
        }
        destroy_request_state(state);
        return;
    }
    if (state->response.relay) {
        if (!start_async_relay_ipc(state, error, sizeof(error))) {
            runtime.invoke_failures++;
            if (!state->connection->closed(state->connection)) {
                send_error(state->connection, 502,
                           "RELAY_INVOKE_UNAVAILABLE",
                           error[0] != '\0' ? error :
                           "failed to start Relay invocation");
            }
            destroy_request_state(state);
        }
        return;
    }
    if (!start_async_backend(state, error, sizeof(error))) {
        if (try_next_route_candidate(state)) return;
        if (state->retry_eligible && state->candidates.count > 0U) {
            runtime.retry_exhausted++;
        }
        runtime.invoke_failures++;
        if (!state->connection->closed(state->connection)) {
            send_error(
                state->connection, 502,
                state->backend_started ? "BACKEND_UNAVAILABLE" :
                                         "BACKEND_ENDPOINT_REJECTED",
                error[0] != '\0' ? error :
                    "failed to start backend invocation");
        }
        destroy_request_state(state);
    }
}

static void ipc_deadline_callback(
    struct ev_loop *loop,
    ev_timer *watcher,
    int revents
)
{
    struct gateway_request_state *state = watcher->data;

    (void)loop;
    (void)revents;
    runtime.ipc_timeouts++;
    if (state->relay_ipc) runtime.invoke_failures++;
    if (state->relay_ipc && state->stream) runtime.stream_failures++;
    (void)gateway_ipc_machine_transition(
        &state->ipc_machine, GATEWAY_IPC_DEADLINE_EXPIRED);
    if (state->relay_ipc && state->stream && state->stream_head_sent &&
        !state->connection->closed(state->connection)) {
        state->connection->userdata = NULL;
        state->connection->close(state->connection);
    } else if (!state->connection->closed(state->connection)) {
        send_error(state->connection, 503, "CONTROL_PLANE_TIMEOUT",
                   "agentd IPC deadline expired");
    }
    destroy_request_state(state);
}

static bool start_async_ipc(
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
)
{
    struct agent_ipc_candidates_request candidates_request;
    struct sockaddr_un address;
    struct ev_loop *loop = state->connection->get_loop(state->connection);
    ev_tstamp timeout;
    int connect_result;

    memset(&candidates_request, 0, sizeof(candidates_request));
    candidates_request.lookup = state->request;
    candidates_request.max_candidates = state->resume_requested
        ? AGENT_IPC_MAX_CANDIDATES : config.max_route_attempts;
    if ((state->operation == GATEWAY_OPERATION_REGISTER &&
         agent_ipc_encode_register_request(
             &state->registration, state->frame, sizeof(state->frame),
             &state->frame_length) != AGENT_IPC_OK) ||
        (state->operation == GATEWAY_OPERATION_RENEW &&
         agent_ipc_encode_renew_request(
             &state->renewal, state->frame, sizeof(state->frame),
             &state->frame_length) != AGENT_IPC_OK) ||
        (state->operation == GATEWAY_OPERATION_UNREGISTER &&
         agent_ipc_encode_unregister_request(
             &state->removal, state->frame, sizeof(state->frame),
             &state->frame_length) != AGENT_IPC_OK) ||
        (!operation_is_registration(state->operation) &&
         state->candidate_lookup &&
         agent_ipc_encode_candidates_request(
             &candidates_request, state->frame, sizeof(state->frame),
             &state->frame_length) != AGENT_IPC_OK) ||
        (!operation_is_registration(state->operation) &&
         !state->candidate_lookup &&
         agent_ipc_encode_lookup_request(
             &state->request, state->frame, sizeof(state->frame),
             &state->frame_length) != AGENT_IPC_OK)) {
        snprintf(error, error_capacity, "failed to encode IPC request");
        return false;
    }

    state->ipc_fd = socket(AF_UNIX,
                           SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                           0);
    if (state->ipc_fd < 0) {
        snprintf(error, error_capacity, "failed to create IPC socket");
        return false;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (!copy_text(address.sun_path, sizeof(address.sun_path),
                   config.ipc_socket)) {
        snprintf(error, error_capacity, "invalid IPC socket path");
        return false;
    }

    gateway_ipc_machine_init(&state->ipc_machine);
    ev_io_init(&state->ipc_watcher, ipc_event_callback,
               state->ipc_fd, EV_WRITE);
    state->ipc_watcher.data = state;
    timeout = (ev_tstamp)config.ipc_timeout_ms / 1000.0;
    ev_timer_init(&state->deadline_watcher, ipc_deadline_callback,
                  timeout, 0.0);
    state->deadline_watcher.data = state;
    state->connection->incref(state->connection);
    state->connection_retained = true;
    ev_timer_start(loop, &state->deadline_watcher);

    connect_result = connect(state->ipc_fd,
                             (struct sockaddr *)&address,
                             sizeof(address));
    if (connect_result == 0) {
        if (!gateway_ipc_machine_transition(
                &state->ipc_machine, GATEWAY_IPC_CONNECTED) ||
            !try_send_ipc_request(state)) {
            snprintf(error, error_capacity, "agentd IPC send failed");
            return false;
        }
        return true;
    }
    if (errno != EINPROGRESS && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        snprintf(error, error_capacity, "agentd IPC is unavailable");
        return false;
    }
    ev_io_start(loop, &state->ipc_watcher);
    return true;
}

static bool start_async_relay_ipc(
    struct gateway_request_state *state,
    char *error,
    size_t error_capacity
)
{
    struct agent_ipc_invoke_request request;
    struct agent_forwarding_context forwarding_context;
    enum agent_forwarding_result forwarding_result;
    struct sockaddr_un address;
    struct ev_loop *loop = state->connection->get_loop(state->connection);
    ev_tstamp timeout;
    int connect_result;

    if (state->forward_body_length > AGENT_IPC_MAX_INVOKE_BODY) {
        snprintf(error, error_capacity,
                 "Relay invoke body exceeds the %u-byte bound",
                 AGENT_IPC_MAX_INVOKE_BODY);
        return false;
    }
    memset(&request, 0, sizeof(request));
    request.request_id = state->request.request_id;
    request.timeout_ms = state->stream
        ? config.stream_idle_timeout_ms
        : state->effective_backend_timeout_ms;
    request.streaming = state->stream;
    request.hop_limit = state->forwarded_hop_limit;
    request.max_cost_microunits = state->request.max_cost_microunits;
    request.max_latency_ms = state->request.max_latency_ms != 0U
        ? state->request.max_latency_ms : state->effective_backend_timeout_ms;
    request.body_length = state->forward_body_length;
    memcpy(request.body, state->forward_body, request.body_length);
    if (!copy_text(request.route_id, sizeof(request.route_id),
                   state->response.route_id) ||
        !copy_text(request.relay_peer_id, sizeof(request.relay_peer_id),
                   state->response.relay_peer_id) ||
        !copy_text(request.target_router_id,
                   sizeof(request.target_router_id),
                   state->response.target_router_id) ||
        !copy_text(request.intent, sizeof(request.intent),
                   state->request.intent) ||
        !copy_text(request.task_id, sizeof(request.task_id),
                   state->task_id) ||
        !copy_text(request.source_agent, sizeof(request.source_agent),
                   state->source_agent) ||
        (state->request.target_agent[0] != '\0' &&
         !copy_text(request.target_agent, sizeof(request.target_agent),
                    state->request.target_agent)) ||
        !copy_text(request.tenant, sizeof(request.tenant),
                   state->request.tenant) ||
        !copy_text(request.region, sizeof(request.region),
                   state->request.region[0] != '\0' ?
                       state->request.region : "*")) {
        snprintf(error, error_capacity,
                 "failed to construct bounded Relay IPC request");
        return false;
    }
    if (config.forwarding_assertion_enabled) {
        if (!state->identity.verified) {
            runtime.forwarding_assertion_failures++;
            snprintf(error, error_capacity,
                     "Relay forwarding requires authenticated identity");
            return false;
        }
        memset(&forwarding_context, 0, sizeof(forwarding_context));
        forwarding_context.source_router_id = config.forwarding_issuer;
        forwarding_context.target_router_id = request.target_router_id;
        forwarding_context.source_agent = request.source_agent;
        forwarding_context.tenant = request.tenant;
        forwarding_context.intent = request.intent;
        forwarding_context.task_id = request.task_id;
        forwarding_context.hop_limit = request.hop_limit;
        forwarding_context.body = request.body;
        forwarding_context.body_length = request.body_length;
        forwarding_result = agent_forwarding_issue(
            &forwarding_signer, &forwarding_context,
            wall_clock_seconds(), request.forwarding_assertion,
            sizeof(request.forwarding_assertion));
        if (forwarding_result != AGENT_FORWARDING_OK) {
            runtime.forwarding_assertion_failures++;
            snprintf(error, error_capacity,
                     "failed to issue forwarding assertion: %s",
                     agent_forwarding_result_name(forwarding_result));
            return false;
        }
        runtime.forwarding_assertions_issued++;
    }
    if ((state->stream
             ? agent_ipc_encode_stream_request(
                   &request, state->frame, sizeof(state->frame),
                   &state->frame_length)
             : agent_ipc_encode_invoke_request(
                   &request, state->frame, sizeof(state->frame),
                   &state->frame_length)) != AGENT_IPC_OK) {
        snprintf(error, error_capacity,
                 "failed to encode bounded Relay IPC request");
        return false;
    }
    state->relay_ipc = true;
    state->ipc_fd = socket(AF_UNIX,
                           SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                           0);
    if (state->ipc_fd < 0) {
        snprintf(error, error_capacity, "failed to create Relay IPC socket");
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (!copy_text(address.sun_path, sizeof(address.sun_path),
                   config.ipc_socket)) {
        snprintf(error, error_capacity, "invalid IPC socket path");
        return false;
    }
    gateway_ipc_machine_init(&state->ipc_machine);
    ev_io_init(&state->ipc_watcher, ipc_event_callback,
               state->ipc_fd, EV_WRITE);
    state->ipc_watcher.data = state;
    timeout = (ev_tstamp)(state->stream
        ? config.stream_idle_timeout_ms
        : state->effective_backend_timeout_ms) / 1000.0;
    ev_timer_init(&state->deadline_watcher, ipc_deadline_callback,
                  timeout, 0.0);
    state->deadline_watcher.data = state;
    ev_timer_start(loop, &state->deadline_watcher);
    connect_result = connect(state->ipc_fd,
                             (struct sockaddr *)&address,
                             sizeof(address));
    if (connect_result == 0) {
        if (!gateway_ipc_machine_transition(
                &state->ipc_machine, GATEWAY_IPC_CONNECTED) ||
            !try_send_ipc_request(state)) {
            snprintf(error, error_capacity, "Relay IPC send failed");
            return false;
        }
        return true;
    }
    if (errno != EINPROGRESS && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        snprintf(error, error_capacity, "agentd Relay IPC is unavailable");
        return false;
    }
    ev_io_start(loop, &state->ipc_watcher);
    return true;
}

static bool content_type_is_json(struct uh_connection *connection)
{
    struct uh_str content_type =
        connection->get_header(connection, "Content-Type");
    static const char json_type[] = "application/json";
    static const char nexus_type[] =
        "application/vnd.nexus.agent-envelope+json";

    return (content_type.len >= sizeof(json_type) - 1U &&
            strncasecmp(content_type.p, json_type,
                        sizeof(json_type) - 1U) == 0 &&
            (content_type.len == sizeof(json_type) - 1U ||
             content_type.p[sizeof(json_type) - 1U] == ';')) ||
           (content_type.len >= sizeof(nexus_type) - 1U &&
            strncasecmp(content_type.p, nexus_type,
                        sizeof(nexus_type) - 1U) == 0 &&
            (content_type.len == sizeof(nexus_type) - 1U ||
             content_type.p[sizeof(nexus_type) - 1U] == ';'));
}

static bool accepts_event_stream(struct uh_connection *connection)
{
    struct uh_str accept = connection->get_header(connection, "Accept");
    static const char expected[] = "text/event-stream";
    size_t index;

    if (accept.p == NULL || accept.len < sizeof(expected) - 1U) return false;
    for (index = 0U; index + sizeof(expected) - 1U <= accept.len; index++) {
        if (strncasecmp(accept.p + index, expected,
                        sizeof(expected) - 1U) == 0) return true;
    }
    return false;
}

static bool authenticate_request(
    struct uh_connection *connection,
    struct gateway_request_state *state
)
{
    struct uh_str authorization =
        connection->get_header(connection, "Authorization");
    struct uh_str transaction_header =
        connection->get_header(connection, "Txn-Token");
    struct agent_auth_claims claims;
    enum agent_auth_result claim_result;
    enum agent_jwt_result jwt_result;
    enum agent_replay_result replay_result;
    static const char bearer_prefix[] = "Bearer ";
    const char *token;
    size_t token_length;
    uint64_t now_seconds;
    bool transaction_token = false;

    if (!config.jwt_required && authorization.p == NULL &&
        transaction_header.p == NULL) {
        return true;
    }
    if (transaction_header.p != NULL) {
        if (transaction_header.len == 0U ||
            authorization.p != NULL ||
            transaction_header.len > AGENT_JWT_MAX_TOKEN_LEN) {
            runtime.authentication_failures++;
            runtime.last_auth_failure_recorded = true;
            snprintf(runtime.last_auth_failure_stage,
                     sizeof(runtime.last_auth_failure_stage),
                     "%s", "transaction_header");
            runtime.last_auth_jwt_result = AGENT_JWT_INVALID_ARGUMENT;
            runtime.last_auth_claim_result = AGENT_AUTH_INVALID_ARGUMENT;
            return false;
        }
        token = transaction_header.p;
        token_length = transaction_header.len;
        transaction_token = true;
    } else {
        if (authorization.p == NULL ||
            authorization.len <= sizeof(bearer_prefix) - 1U ||
            authorization.len >
                AGENT_JWT_MAX_TOKEN_LEN +
                    sizeof(bearer_prefix) - 1U ||
            strncmp(authorization.p, bearer_prefix,
                    sizeof(bearer_prefix) - 1U) != 0) {
            runtime.authentication_failures++;
            runtime.last_auth_failure_recorded = true;
            snprintf(runtime.last_auth_failure_stage,
                     sizeof(runtime.last_auth_failure_stage),
                     "%s", "authorization_header");
            runtime.last_auth_jwt_result = AGENT_JWT_INVALID_ARGUMENT;
            runtime.last_auth_claim_result = AGENT_AUTH_INVALID_ARGUMENT;
            return false;
        }
        token = authorization.p + sizeof(bearer_prefix) - 1U;
        token_length =
            authorization.len - (sizeof(bearer_prefix) - 1U);
    }

    now_seconds = wall_clock_seconds();
    if (!transaction_token && token_length > 5U &&
        strncmp(token, "nls1.", 5U) == 0) {
        struct agent_lan_session_claims session_claims;
        enum agent_lan_session_result session_result;
        char session_token[AGENT_LAN_SESSION_TOKEN_MAX];

        if (!capture_trusted_lan_source(
                connection, state->lan_source_address)) {
            runtime.authentication_failures++;
            runtime.lan_session_rejected++;
            audit_lan_bootstrap("rejected", "untrusted", "session_source");
            return false;
        }
        memset(&session_claims, 0, sizeof(session_claims));
        if (token_length >= sizeof(session_token)) {
            session_result = AGENT_LAN_SESSION_MALFORMED;
        } else {
            memcpy(session_token, token, token_length);
            session_token[token_length] = '\0';
            session_result = agent_lan_session_verify(
                config.lan_session_key, session_token,
                state->lan_source_address, now_seconds, &session_claims);
            memset(session_token, 0, sizeof(session_token));
        }
        if (session_result != AGENT_LAN_SESSION_OK ||
            !copy_text(state->identity.tenant,
                       sizeof(state->identity.tenant), session_claims.tenant) ||
            !copy_text(state->identity.source_agent,
                       sizeof(state->identity.source_agent),
                       session_claims.origin) ||
            !copy_text(state->authenticated_scope,
                       sizeof(state->authenticated_scope),
                       AGENT_LAN_SESSION_SCOPE)) {
            runtime.authentication_failures++;
            runtime.lan_session_rejected++;
            audit_lan_bootstrap(
                "rejected", state->lan_source_address,
                agent_lan_session_result_name(session_result));
            return false;
        }
        state->identity.verified = true;
        state->lan_session = true;
        runtime.authentication_successes++;
        runtime.lan_session_authenticated++;
        return true;
    }
    memset(&claims, 0, sizeof(claims));
    jwt_result = agent_jwt_verify(
        &jwt_verifier,
        token,
        token_length,
        now_seconds,
        &claims,
        &claim_result);
    if (jwt_result != AGENT_JWT_OK) {
        runtime.authentication_failures++;
        runtime.last_auth_failure_recorded = true;
        snprintf(runtime.last_auth_failure_stage,
                 sizeof(runtime.last_auth_failure_stage),
                 "%s", "jwt_verify");
        runtime.last_auth_jwt_result = jwt_result;
        runtime.last_auth_claim_result =
            jwt_result == AGENT_JWT_CLAIMS_INVALID
                ? claim_result
                : AGENT_AUTH_INVALID_ARGUMENT;
        return false;
    }
    if (claims.transaction_token != transaction_token ||
        !agent_auth_identity_from_claims(
            &claims, &state->identity)) {
        runtime.authentication_failures++;
        runtime.last_auth_failure_recorded = true;
        snprintf(runtime.last_auth_failure_stage,
                 sizeof(runtime.last_auth_failure_stage),
                 "%s", "identity_binding");
        runtime.last_auth_jwt_result = AGENT_JWT_OK;
        runtime.last_auth_claim_result = AGENT_AUTH_OK;
        return false;
    }
    if (transaction_token) {
        replay_result = agent_replay_cache_check_and_store(
            &replay_cache,
            claims.issuer,
            claims.transaction_id,
            claims.expires_at,
            now_seconds);
        if (replay_result != AGENT_REPLAY_ACCEPTED) {
            if (replay_result == AGENT_REPLAY_DETECTED) {
                runtime.transaction_replays_rejected++;
            } else {
                runtime.transaction_cache_rejections++;
            }
            runtime.authentication_failures++;
            runtime.last_auth_failure_recorded = true;
            snprintf(runtime.last_auth_failure_stage,
                     sizeof(runtime.last_auth_failure_stage),
                     "%s", "transaction_replay");
            runtime.last_auth_jwt_result = AGENT_JWT_OK;
            runtime.last_auth_claim_result = AGENT_AUTH_OK;
            return false;
        }
        runtime.transaction_tokens_accepted++;
    }
    if (!copy_text(state->authenticated_scope,
                   sizeof(state->authenticated_scope), claims.scope)) {
        runtime.authentication_failures++;
        runtime.last_auth_failure_recorded = true;
        snprintf(runtime.last_auth_failure_stage,
                 sizeof(runtime.last_auth_failure_stage),
                 "%s", "scope_copy");
        runtime.last_auth_jwt_result = AGENT_JWT_OK;
        runtime.last_auth_claim_result = AGENT_AUTH_OK;
        return false;
    }
    if (claims.target_agent[0] != '\0' &&
        !copy_text(state->authenticated_target_agent,
                   sizeof(state->authenticated_target_agent),
                   claims.target_agent)) {
        runtime.authentication_failures++;
        runtime.last_auth_failure_recorded = true;
        snprintf(runtime.last_auth_failure_stage,
                 sizeof(runtime.last_auth_failure_stage),
                 "%s", "target_copy");
        runtime.last_auth_jwt_result = AGENT_JWT_OK;
        runtime.last_auth_claim_result = AGENT_AUTH_OK;
        return false;
    }
    runtime.authentication_successes++;
    return true;
}

static bool copy_internal_header(
    struct uh_str header,
    char *target,
    size_t capacity,
    bool allow_missing
)
{
    size_t index;

    if (target == NULL || capacity == 0U) return false;
    target[0] = '\0';
    if (header.p == NULL) return allow_missing;
    if (header.len == 0U) return allow_missing;
    if (header.len >= capacity) return false;
    for (index = 0U; index < header.len; index++) {
        unsigned char character = (unsigned char)header.p[index];

        if (character < 0x21U || character > 0x7eU) return false;
    }
    memcpy(target, header.p, header.len);
    target[header.len] = '\0';
    return true;
}

static bool parse_internal_envelope_target(
    const char *body,
    size_t body_length,
    char target_agent[AGENT_IPC_URI_LEN]
)
{
    struct json_tokener *tokener;
    struct json_object *root = NULL;
    struct json_object *value;
    enum json_tokener_error parse_error;
    const char *body_target;
    bool valid = false;

    if (body == NULL || body_length == 0U || target_agent == NULL ||
        body_length > (size_t)INT_MAX) return false;
    target_agent[0] = '\0';
    tokener = json_tokener_new_ex(16);
    if (tokener == NULL) return false;
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT |
                           JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tokener, body, (int)body_length);
    parse_error = json_tokener_get_error(tokener);
    if (parse_error != json_tokener_success || root == NULL ||
        json_tokener_get_parse_end(tokener) != body_length ||
        !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "version", &value) ||
        !json_object_is_type(value, json_type_string) ||
        strcmp(json_object_get_string(value), "1.0") != 0 ||
        !json_object_object_get_ex(root, "payload", &value) ||
        json_object_is_type(value, json_type_null)) {
        goto done;
    }
    if (!json_object_object_get_ex(root, "target_agent", &value) ||
        !json_object_is_type(value, json_type_string)) goto done;
    body_target = json_object_get_string(value);
    valid = body_target != NULL && body_target[0] != '\0' &&
            copy_text(target_agent, AGENT_IPC_URI_LEN, body_target);

done:
    if (root != NULL) json_object_put(root);
    json_tokener_free(tokener);
    return valid;
}

static void internal_invoke_handler(
    struct uh_connection *connection,
    int event
)
{
    struct gateway_request_state *state;
    struct agent_invoke_endpoint selected_endpoint;
    enum agent_invoke_internal_selection_result selection_result;
    struct uh_str token;
    struct uh_str body;
    char selected_origin[AGENT_IPC_URI_LEN] = {0};
    char selected_target[AGENT_IPC_URI_LEN] = {0};
    char envelope_target[AGENT_IPC_URI_LEN] = {0};
    char timeout_text[16] = {0};
    uint32_t internal_timeout_ms = 0U;
    uint64_t now_ms = 0U;
    uint64_t lease_remaining_ms = 0U;
    char error[256] = {0};

    if (event == UH_EV_HEAD_COMPLETE) {
        release_request(connection);
        state = calloc(1U, sizeof(*state));
        if (state == NULL) {
            send_error(connection, 500, "OUT_OF_MEMORY",
                       "failed to allocate internal invoke state");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        state->connection = connection;
        state->invoke = true;
        state->internal_invoke = true;
        state->operation = GATEWAY_OPERATION_INTERNAL_INVOKE;
        state->ipc_fd = -1;
        state->backend_fd = -1;
        gateway_ipc_machine_init(&state->ipc_machine);
        connection->userdata = state;
        runtime.internal_invoke_requests++;
        if (!config.internal_invoke_enabled) {
            reject_request(connection, 404, "INTERNAL_INVOKE_DISABLED",
                           "internal invoke is disabled");
            return;
        }
        token = connection->get_header(
            connection, AGENT_INVOKE_INTERNAL_TOKEN_HEADER);
        if (!internal_invoke_token_matches_digest(token.p, token.len)) {
            runtime.internal_invoke_auth_rejections++;
            reject_request(connection, 403, "INTERNAL_AUTHENTICATION_FAILED",
                           "internal caller authentication failed");
            return;
        }
        if (!config.invoke_enabled) {
            reject_request(connection, 404, "INVOKE_DISABLED",
                           "Agent invocation is disabled");
            return;
        }
        if (!agent_rate_limiter_allow(&runtime.rate_limiter,
                                      monotonic_ms())) {
            runtime.rate_rejections++;
            reject_request(connection, 429, "RATE_LIMITED",
                           "gateway request rate exceeded");
            return;
        }
        if (runtime.active_requests >= config.max_inflight) {
            runtime.concurrency_rejections++;
            reject_request(connection, 503, "CONCURRENCY_LIMITED",
                           "gateway concurrency limit exceeded");
            return;
        }
        state->counted = true;
        runtime.active_requests++;
        runtime.admitted_requests++;
        if (strcmp(connection->get_method_str(connection), "POST") != 0) {
            reject_request(connection, 405, "METHOD_NOT_ALLOWED",
                           "use POST for this endpoint");
            return;
        }
        if (connection->get_content_length(connection) == 0U ||
            connection->get_content_length(connection) >
                config.max_envelope_bytes) {
            reject_request(connection, 413, "ENVELOPE_TOO_LARGE",
                           "Envelope exceeds the configured bound");
            return;
        }
        connection->check_expect_100_continue(connection);
        return;
    }
    if (event != UH_EV_COMPLETE) return;
    if (connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    state = connection->userdata;
    if (state == NULL) {
        send_error(connection, 500, "REQUEST_STATE_LOST",
                   "internal invoke state is unavailable");
        return;
    }
    if (!content_type_is_json(connection) ||
        !copy_internal_header(
            connection->get_header(connection, "X-Nexus-Route-Id"),
            state->response.route_id,
            sizeof(state->response.route_id), false) ||
        !copy_internal_header(
            connection->get_header(
                connection, AGENT_INVOKE_INTERNAL_ENDPOINT_HEADER),
            state->response.endpoint,
            sizeof(state->response.endpoint), false) ||
        !copy_internal_header(
            connection->get_header(
                connection, AGENT_INVOKE_INTERNAL_ORIGIN_HEADER),
            selected_origin, sizeof(selected_origin), false) ||
        !copy_internal_header(
            connection->get_header(
                connection, AGENT_INVOKE_INTERNAL_TARGET_HEADER),
            selected_target, sizeof(selected_target), false) ||
        !copy_internal_header(
            connection->get_header(
                connection, AGENT_INVOKE_INTERNAL_TIMEOUT_HEADER),
            timeout_text, sizeof(timeout_text), false) ||
        !parse_u32(timeout_text, &internal_timeout_ms) ||
        internal_timeout_ms < 10U || internal_timeout_ms > 30000U) {
        runtime.internal_invoke_route_rejections++;
        send_error(connection, 400, "INVALID_INTERNAL_INVOKE",
                   "internal route metadata is invalid");
        goto done;
    }
    body = connection->get_body(connection);
    if (!parse_internal_envelope_target(
            body.p, body.len, envelope_target)) {
        runtime.internal_invoke_route_rejections++;
        send_error(connection, 400, "INVALID_INTERNAL_ENVELOPE",
                   "internal Envelope target is missing or invalid");
        goto done;
    }
    now_ms = monotonic_ms();
    selection_result = agent_invoke_internal_selection_validate(
        &dynamic_backend_maps, state->response.route_id,
        state->response.endpoint, selected_origin, selected_target,
        envelope_target, now_ms, &selected_endpoint,
        &state->internal_dynamic_map);
    if (selection_result != AGENT_INVOKE_INTERNAL_SELECTION_OK) {
        runtime.internal_invoke_route_rejections++;
        send_error(
            connection,
            selection_result == AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH
                ? 403 : 409,
            selection_result == AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH
                ? "INTERNAL_TARGET_MISMATCH" : "INTERNAL_ROUTE_NOT_LEASED",
            selection_result == AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH
                ? "selected route is not bound to the Envelope target"
                : "selected HTTPS route has no matching active lease");
        goto done;
    }
    state->internal_dynamic_map_pinned = true;
    if (state->internal_dynamic_map.expires_at_ms <= now_ms) {
        runtime.internal_invoke_route_rejections++;
        send_error(connection, 409, "INTERNAL_ROUTE_NOT_LEASED",
                   "selected HTTPS route lease has expired");
        goto done;
    }
    lease_remaining_ms =
        state->internal_dynamic_map.expires_at_ms - now_ms;
    if (lease_remaining_ms < 10U) {
        runtime.internal_invoke_route_rejections++;
        send_error(connection, 409, "INTERNAL_ROUTE_NOT_LEASED",
                   "selected HTTPS route lease has insufficient lifetime");
        goto done;
    }
    if (!copy_text(state->response.origin,
                   sizeof(state->response.origin), selected_origin)) {
        runtime.internal_invoke_route_rejections++;
        send_error(connection, 400, "INVALID_INTERNAL_INVOKE",
                   "selected origin exceeds the gateway bound");
        goto done;
    }
    state->forward_body = malloc(body.len + 1U);
    if (state->forward_body == NULL) {
        send_error(connection, 500, "OUT_OF_MEMORY",
                   "failed to retain internal Envelope");
        goto done;
    }
    memcpy(state->forward_body, body.p, body.len);
    state->forward_body[body.len] = '\0';
    state->forward_body_length = body.len;
    state->response.found = true;
    state->effective_backend_timeout_ms = internal_timeout_ms <
        config.backend_timeout_ms ? internal_timeout_ms :
                                    config.backend_timeout_ms;
    if (lease_remaining_ms < state->effective_backend_timeout_ms) {
        state->effective_backend_timeout_ms =
            (uint32_t)lease_remaining_ms;
    }
    state->backend_deadline_ms = now_ms +
        state->effective_backend_timeout_ms;
    connection->incref(connection);
    state->connection_retained = true;
    if (!start_async_backend(state, error, sizeof(error))) {
        runtime.internal_invoke_failures++;
        runtime.invoke_failures++;
        send_error(connection, 502,
                   state->backend_started ? "BACKEND_UNAVAILABLE" :
                                            "BACKEND_ENDPOINT_REJECTED",
                   error[0] != '\0' ? error :
                       "failed to start leased HTTPS backend");
        destroy_request_state(state);
    }
    return;

done:
    release_request(connection);
}

static void gateway_request_handler(
    struct uh_connection *connection,
    int event,
    bool invoke,
    bool stream
)
{
    struct gateway_request_state *state;
    struct uh_str body;
    char error[256] = {0};
    bool deadline_expired = false;

    if (event == UH_EV_HEAD_COMPLETE) {
        release_request(connection);
        state = calloc(1U, sizeof(*state));
        if (state == NULL) {
            send_error(connection, 500, "OUT_OF_MEMORY",
                       "failed to allocate request state");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        state->connection = connection;
        state->invoke = invoke;
        state->stream = stream;
        state->operation = stream ? GATEWAY_OPERATION_STREAM :
            invoke ? GATEWAY_OPERATION_INVOKE : GATEWAY_OPERATION_ROUTE;
        state->ipc_fd = -1;
        state->backend_fd = -1;
        gateway_ipc_machine_init(&state->ipc_machine);
        connection->userdata = state;
        if (!capture_public_ingress(connection, state)) {
            reject_request(connection, 400, "INVALID_PUBLIC_INGRESS",
                           "public ingress metadata is invalid");
            return;
        }
        if (state->public_ingress && !invoke) {
            runtime.public_ingress_rejections++;
            reject_request(connection, 404, "PUBLIC_INVOKE_ONLY",
                           "public Agent IPv6 only accepts invoke endpoints");
            return;
        }
        if (invoke && !config.invoke_enabled) {
            reject_request(connection, 404, "INVOKE_DISABLED",
                           "Agent invocation is disabled");
            return;
        }
        if (stream && !config.stream_enabled) {
            reject_request(connection, 404, "STREAM_DISABLED",
                           "Agent streaming is disabled");
            return;
        }
        if (!agent_rate_limiter_allow(&runtime.rate_limiter,
                                      monotonic_ms())) {
            runtime.rate_rejections++;
            reject_request(connection, 429, "RATE_LIMITED",
                           "gateway request rate exceeded");
            return;
        }
        if (runtime.active_requests >= config.max_inflight) {
            runtime.concurrency_rejections++;
            reject_request(connection, 503, "CONCURRENCY_LIMITED",
                           "gateway concurrency limit exceeded");
            return;
        }
        state->counted = true;
        runtime.active_requests++;
        runtime.admitted_requests++;

        if (strcmp(connection->get_method_str(connection), "POST") != 0) {
            reject_request(connection, 405, "METHOD_NOT_ALLOWED",
                           "use POST for this endpoint");
            return;
        }
        if (stream && !accepts_event_stream(connection)) {
            reject_request(connection, 406, "SSE_REQUIRED",
                           "Accept must include text/event-stream");
            return;
        }
        if (connection->get_content_length(connection) == 0U ||
            connection->get_content_length(connection) >
                config.max_envelope_bytes) {
            reject_request(connection, 413, "ENVELOPE_TOO_LARGE",
                           "Envelope exceeds the configured bound");
            return;
        }
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
        send_error(connection, 500, "REQUEST_STATE_LOST",
                   "gateway request state is unavailable");
        return;
    }
    state = connection->userdata;
    if (!authenticate_request(connection, state)) {
        send_error(connection, 401, "AUTHENTICATION_REQUIRED",
                   "a valid access or transaction token is required");
        goto done;
    }
    if (invoke && config.jwt_required &&
        !agent_auth_scope_contains(state->authenticated_scope,
                                   "agent.invoke")) {
        send_error(connection, 403, "INSUFFICIENT_SCOPE",
                   "the agent.invoke scope is required");
        goto done;
    }
    if (!content_type_is_json(connection)) {
        send_error(connection, 415, "UNSUPPORTED_MEDIA_TYPE",
                   "Content-Type must be an Agent Envelope JSON type");
        goto done;
    }

    body = connection->get_body(connection);
    if (!parse_envelope(body.p, body.len, invoke, &state->request,
                        &state->identity, state->lan_session,
                        &state->lan_identity_mismatch,
                        &state->tenant_overridden,
                        state->task_id,
                        state->source_agent,
                        &state->forwarded_hop_limit,
                        &state->forward_body,
                        &state->forward_body_length,
                        &state->effective_backend_timeout_ms,
                        &state->interactive_requested,
                        state->request_deadline,
                        &state->retry_requested,
                        &state->resume_requested,
                        &state->resume_from_event_id,
                        &deadline_expired,
                        error, sizeof(error))) {
        if (state->lan_identity_mismatch) {
            runtime.lan_session_rejected++;
            audit_lan_bootstrap(
                "rejected", state->lan_source_address, "identity_mismatch");
        }
        const char *invalid_code = strstr(error, "UTF-8") != NULL
            ? "INVALID_UTF8_JSON" : "INVALID_ENVELOPE";

        send_error(connection,
                   state->lan_identity_mismatch ? 401 :
                   (deadline_expired ? 408 : 400),
                   state->lan_identity_mismatch ? "AUTHENTICATION_REQUIRED" :
                   (deadline_expired ? "DEADLINE_EXPIRED" :
                                       invalid_code),
                   error);
        goto done;
    }
    if (state->source_agent[0] != '\0' &&
        !copy_text(state->request.source_agent,
                   sizeof(state->request.source_agent),
                   state->source_agent)) {
        send_error(connection, 400, "INVALID_ENVELOPE",
                   "source_agent exceeds policy routing bound");
        goto done;
    }
    if (state->public_ingress &&
        !copy_text(state->request.public_ipv6,
                   sizeof(state->request.public_ipv6),
                   state->public_ipv6)) {
        send_error(connection, 400, "INVALID_PUBLIC_INGRESS",
                   "public IPv6 destination exceeds the IPC bound");
        goto done;
    }
    if (state->public_ingress && state->retry_requested) {
        send_error(connection, 400, "PUBLIC_ROUTE_PINNED",
                   "public IPv6 invokes cannot request route failover");
        goto done;
    }
    if ((state->public_ingress &&
         state->authenticated_target_agent[0] == '\0') ||
        (state->authenticated_target_agent[0] != '\0' &&
         (state->request.target_agent[0] == '\0' ||
          strcmp(state->authenticated_target_agent,
                 state->request.target_agent) != 0))) {
        send_error(connection, 403, "TARGET_AGENT_MISMATCH",
                   "the access token is not valid for this target Agent");
        goto done;
    }
    (void)apply_cloud_tenant_alias(state);
    apply_interactive_backend_timeout(state);
    if (state->stream && state->retry_requested) {
        send_error(connection, 400, "INVALID_ENVELOPE",
                   "P2.7.5 retry is not available for streaming invokes");
        goto done;
    }
    if (state->resume_requested && !state->stream) {
        send_error(connection, 400, "INVALID_ENVELOPE",
                   "resume_from_event_id is only valid for streaming invokes");
        goto done;
    }
    if (state->resume_requested) {
        enum agent_stream_resume_result resume_result;

        if (!config.stream_resume_enabled) {
            send_error(connection, 409, "STREAM_RESUME_DISABLED",
                       "stream resume is disabled on this router");
            goto done;
        }
        resume_result = agent_stream_resume_cache_lookup(
            &stream_resume_cache, state->task_id, state->request.tenant,
            state->source_agent, state->request.intent, monotonic_ms(),
            state->resume_route_id);
        runtime.resume_requests++;
        if (resume_result != AGENT_STREAM_RESUME_OK) {
            runtime.resume_misses++;
            send_error(connection, 404, "STREAM_TASK_NOT_FOUND",
                       "the router no longer retains the original task route");
            goto done;
        }
    }
    state->backend_deadline_ms = monotonic_ms() +
        state->effective_backend_timeout_ms;
    state->retry_eligible = config.retry_enabled &&
        state->retry_requested && !state->stream;
    state->candidate_lookup = state->retry_eligible || state->resume_requested;
    if (state->retry_eligible) runtime.retry_requests++;
    if (!start_async_ipc(state, error, sizeof(error))) {
        fail_async_request(state, error);
    }
    return;

done:
    release_request(connection);
}

static void route_handler(struct uh_connection *connection, int event)
{
    gateway_request_handler(connection, event, false, false);
}

static void invoke_handler(struct uh_connection *connection, int event)
{
    gateway_request_handler(connection, event, true, false);
}

static void stream_handler(struct uh_connection *connection, int event)
{
    gateway_request_handler(connection, event, true, true);
}

static void gateway_registration_handler(
    struct uh_connection *connection,
    int event,
    enum gateway_operation operation
)
{
    struct gateway_request_state *state;
    struct uh_str body;
    char error[256] = {0};

    if (event == UH_EV_HEAD_COMPLETE) {
        release_request(connection);
        state = calloc(1U, sizeof(*state));
        if (state == NULL) {
            send_error(connection, 500, "OUT_OF_MEMORY",
                       "failed to allocate registration state");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        state->connection = connection;
        state->operation = operation;
        state->ipc_fd = -1;
        state->backend_fd = -1;
        gateway_ipc_machine_init(&state->ipc_machine);
        connection->userdata = state;
        if (!config.registration_enabled) {
            reject_request(connection, 404, "REGISTRATION_DISABLED",
                           "Agent Access Proxy registration is disabled");
            return;
        }
        if (!agent_rate_limiter_allow(&runtime.rate_limiter,
                                      monotonic_ms())) {
            runtime.rate_rejections++;
            reject_request(connection, 429, "RATE_LIMITED",
                           "gateway request rate exceeded");
            return;
        }
        if (runtime.active_requests >= config.max_inflight) {
            runtime.concurrency_rejections++;
            reject_request(connection, 503, "CONCURRENCY_LIMITED",
                           "gateway concurrency limit exceeded");
            return;
        }
        state->counted = true;
        runtime.active_requests++;
        runtime.admitted_requests++;
        if (strcmp(connection->get_method_str(connection), "POST") != 0) {
            reject_request(connection, 405, "METHOD_NOT_ALLOWED",
                           "use POST for this endpoint");
            return;
        }
        if (connection->get_content_length(connection) == 0U ||
            connection->get_content_length(connection) >
                config.max_envelope_bytes) {
            reject_request(connection, 413, "REGISTRATION_TOO_LARGE",
                           "registration body exceeds the configured bound");
            return;
        }
        connection->check_expect_100_continue(connection);
        return;
    }
    if (event != UH_EV_COMPLETE) return;
    if (connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    state = connection->userdata;
    if (state == NULL) {
        send_error(connection, 500, "REQUEST_STATE_LOST",
                   "registration state is unavailable");
        return;
    }
    if (!authenticate_request(connection, state)) {
        send_error(connection, 401, "AUTHENTICATION_REQUIRED",
                   "a valid access or transaction token is required");
        goto done;
    }
    if (config.jwt_required &&
        !agent_auth_scope_contains(state->authenticated_scope,
                                   "agent.register")) {
        send_error(connection, 403, "INSUFFICIENT_SCOPE",
                   "the agent.register scope is required");
        goto done;
    }
    if (!content_type_is_json(connection)) {
        send_error(connection, 415, "UNSUPPORTED_MEDIA_TYPE",
                   "Content-Type must be application/json");
        goto done;
    }
    body = connection->get_body(connection);
    if (!parse_registration_operation(body.p, body.len, state,
                                      error, sizeof(error))) {
        if (state->lan_identity_mismatch) {
            runtime.lan_session_rejected++;
            audit_lan_bootstrap(
                "rejected", state->lan_source_address, "identity_mismatch");
        }
        send_error(connection,
                   state->lan_identity_mismatch ? 401 : 400,
                   state->lan_identity_mismatch ? "AUTHENTICATION_REQUIRED" :
                                                  "INVALID_REGISTRATION",
                   error);
        goto done;
    }
    if (!start_async_ipc(state, error, sizeof(error))) {
        fail_async_request(state, error);
    }
    return;

done:
    release_request(connection);
}

static void register_handler(struct uh_connection *connection, int event)
{
    gateway_registration_handler(connection, event,
                                 GATEWAY_OPERATION_REGISTER);
}

static void renew_handler(struct uh_connection *connection, int event)
{
    gateway_registration_handler(connection, event,
                                 GATEWAY_OPERATION_RENEW);
}

static void unregister_handler(struct uh_connection *connection, int event)
{
    gateway_registration_handler(connection, event,
                                 GATEWAY_OPERATION_UNREGISTER);
}

static void health_handler(struct uh_connection *connection, int event)
{
    struct json_object *root;

    if (event != UH_EV_COMPLETE) {
        return;
    }
    if (strcmp(connection->get_method_str(connection), "GET") != 0) {
        send_error(connection, 405, "METHOD_NOT_ALLOWED",
                   "use GET for this endpoint");
        return;
    }
    root = json_object_new_object();
    if (root == NULL) {
        send_error(connection, 500, "OUT_OF_MEMORY",
                   "failed to build health response");
        return;
    }
    json_object_object_add(root, "status", json_object_new_string("ok"));
    json_object_object_add(root, "mode",
                           json_object_new_string(
                               config.remote_backend_enabled ?
                                   "invoke-controlled-http-tls" :
                               config.lan_backend_enabled ?
                                   "invoke-automatic-lan-http" :
                               config.invoke_enabled ?
                                   "invoke-loopback-http" :
                                   "route-only-loopback"));
    json_object_object_add(
        root, "invoke_enabled",
        json_object_new_boolean(config.invoke_enabled));
    json_object_object_add(
        root, "registration_enabled",
        json_object_new_boolean(config.registration_enabled));
    json_object_object_add(
        root, "public_descriptor_enabled",
        json_object_new_boolean(config.public_descriptor_enabled));
    json_object_object_add(
        root, "public_descriptor_ready",
        json_object_new_boolean(
            config.public_descriptor_enabled &&
            (strcmp(config.public_scheme, "http") == 0 ||
             (config.public_tls_server_name[0] != '\0' &&
              config.public_ca_bundle_id[0] != '\0'))));
    if (config.public_descriptor_enabled) {
        json_object_object_add(root, "public_scheme",
            json_object_new_string(config.public_scheme));
        json_object_object_add(root, "public_tls_server_name",
            json_object_new_string(config.public_tls_server_name));
        json_object_object_add(root, "public_ca_bundle_id",
            json_object_new_string(config.public_ca_bundle_id));
        json_object_object_add(root, "public_ingress_port",
            json_object_new_uint64(config.public_ingress_port));
    }
    json_object_object_add(root, "active_requests",
                           json_object_new_uint64(runtime.active_requests));
    json_object_object_add(root, "admitted_requests",
                           json_object_new_uint64(runtime.admitted_requests));
    json_object_object_add(
        root, "rate_rejections",
        json_object_new_uint64(runtime.rate_rejections));
    json_object_object_add(
        root, "concurrency_rejections",
        json_object_new_uint64(runtime.concurrency_rejections));
    json_object_object_add(
        root, "tenant_concurrency_rejections",
        json_object_new_uint64(runtime.tenant_concurrency_rejections));
    json_object_object_add(
        root, "tenant_rate_rejections",
        json_object_new_uint64(runtime.tenant_rate_rejections));
    json_object_object_add(
        root, "tenant_quota_table_rejections",
        json_object_new_uint64(runtime.tenant_quota_table_rejections));
    json_object_object_add(root, "ipc_completed",
                           json_object_new_uint64(runtime.ipc_completed));
    json_object_object_add(root, "ipc_failures",
                           json_object_new_uint64(runtime.ipc_failures));
    json_object_object_add(root, "ipc_timeouts",
                           json_object_new_uint64(runtime.ipc_timeouts));
    json_object_object_add(
        root, "client_disconnects",
        json_object_new_uint64(runtime.client_disconnects));
    json_object_object_add(
        root, "jwt_required",
        json_object_new_boolean(config.jwt_required));
    json_object_object_add(
        root, "jwks_enabled",
        json_object_new_boolean(config.jwt_jwks_file[0] != '\0'));
    json_object_object_add(
        root, "jwt_key_count",
        json_object_new_uint64(
            config.jwt_required ? jwt_verifier.key_count : 0U));
    json_object_object_add(
        root, "authentication_successes",
        json_object_new_uint64(runtime.authentication_successes));
    json_object_object_add(
        root, "authentication_failures",
        json_object_new_uint64(runtime.authentication_failures));
    json_object_object_add(
        root, "lan_bootstrap_enabled",
        json_object_new_boolean(config.lan_bootstrap_enabled));
    json_object_object_add(
        root, "lan_bootstrap_issued",
        json_object_new_uint64(runtime.lan_bootstrap_issued));
    json_object_object_add(
        root, "lan_bootstrap_rejected",
        json_object_new_uint64(runtime.lan_bootstrap_rejected));
    json_object_object_add(
        root, "lan_bootstrap_refreshed",
        json_object_new_uint64(runtime.lan_bootstrap_refreshed));
    json_object_object_add(
        root, "lan_session_authenticated",
        json_object_new_uint64(runtime.lan_session_authenticated));
    json_object_object_add(
        root, "lan_session_rejected",
        json_object_new_uint64(runtime.lan_session_rejected));
    json_object_object_add(
        root, "cloud_tenant_aliases",
        json_object_new_uint64(runtime.cloud_tenant_aliases));
    json_object_object_add(
        root, "cloud_tenant_alias_rejections",
        json_object_new_uint64(runtime.cloud_tenant_alias_rejections));
    json_object_object_add(
        root, "last_auth_failure_stage",
        runtime.last_auth_failure_recorded
            ? json_object_new_string(runtime.last_auth_failure_stage)
            : json_object_new_null());
    json_object_object_add(
        root, "last_auth_jwt_result",
        runtime.last_auth_failure_recorded
            ? json_object_new_string(agent_jwt_result_name(
                  runtime.last_auth_jwt_result))
            : json_object_new_null());
    json_object_object_add(
        root, "last_auth_claim_result",
        runtime.last_auth_failure_recorded
            ? json_object_new_string(agent_auth_result_name(
                  runtime.last_auth_claim_result))
            : json_object_new_null());
    json_object_object_add(
        root, "transaction_tokens_accepted",
        json_object_new_uint64(
            runtime.transaction_tokens_accepted));
    json_object_object_add(
        root, "transaction_replays_rejected",
        json_object_new_uint64(
            runtime.transaction_replays_rejected));
    json_object_object_add(
        root, "transaction_cache_rejections",
        json_object_new_uint64(
            runtime.transaction_cache_rejections));
    json_object_object_add(
        root, "forwarding_assertion_enabled",
        json_object_new_boolean(config.forwarding_assertion_enabled));
    json_object_object_add(
        root, "forwarding_assertions_issued",
        json_object_new_uint64(runtime.forwarding_assertions_issued));
    json_object_object_add(
        root, "forwarding_assertion_failures",
        json_object_new_uint64(runtime.forwarding_assertion_failures));
    json_object_object_add(
        root, "invoke_completed",
        json_object_new_uint64(runtime.invoke_completed));
    json_object_object_add(
        root, "invoke_failures",
        json_object_new_uint64(runtime.invoke_failures));
    json_object_object_add(
        root, "internal_invoke_enabled",
        json_object_new_boolean(config.internal_invoke_enabled));
    json_object_object_add(
        root, "internal_invoke_ready",
        json_object_new_boolean(
            config.internal_invoke_enabled &&
            config.internal_invoke_token_digest[0] != '\0' &&
            agent_invoke_internal_generation_is_valid(
                config.internal_invoke_generation)));
    json_object_object_add(
        root, "internal_invoke_generation",
        json_object_new_string(
            config.internal_invoke_enabled
                ? config.internal_invoke_generation : ""));
    json_object_object_add(
        root, "internal_invoke_requests",
        json_object_new_uint64(runtime.internal_invoke_requests));
    json_object_object_add(
        root, "internal_invoke_auth_rejections",
        json_object_new_uint64(runtime.internal_invoke_auth_rejections));
    json_object_object_add(
        root, "internal_invoke_route_rejections",
        json_object_new_uint64(runtime.internal_invoke_route_rejections));
    json_object_object_add(
        root, "internal_invoke_completed",
        json_object_new_uint64(runtime.internal_invoke_completed));
    json_object_object_add(
        root, "internal_invoke_failures",
        json_object_new_uint64(runtime.internal_invoke_failures));
    json_object_object_add(
        root, "backend_timeouts",
        json_object_new_uint64(runtime.backend_timeouts));
    json_object_object_add(
        root, "backend_endpoint_rejections",
        json_object_new_uint64(runtime.backend_endpoint_rejections));
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
    json_object_object_add(
        root, "stream_resume_enabled",
        json_object_new_boolean(config.stream_resume_enabled));
    json_object_object_add(
        root, "stream_resume_capacity",
        json_object_new_uint64(config.stream_resume_capacity));
    json_object_object_add(
        root, "stream_resume_ttl_seconds",
        json_object_new_uint64(config.stream_resume_ttl_seconds));
    json_object_object_add(
        root, "resume_routes_stored",
        json_object_new_uint64(runtime.resume_routes_stored));
    json_object_object_add(
        root, "resume_requests",
        json_object_new_uint64(runtime.resume_requests));
    json_object_object_add(
        root, "resume_misses",
        json_object_new_uint64(runtime.resume_misses));
    json_object_object_add(
        root, "resume_route_unavailable",
        json_object_new_uint64(runtime.resume_route_unavailable));
    json_object_object_add(
        root, "resume_capacity_rejections",
        json_object_new_uint64(runtime.resume_capacity_rejections));
    json_object_object_add(
        root, "relay_streams_started",
        json_object_new_uint64(runtime.relay_streams_started));
    json_object_object_add(
        root, "relay_streams_completed",
        json_object_new_uint64(runtime.relay_streams_completed));
    json_object_object_add(
        root, "relay_stream_frames",
        json_object_new_uint64(runtime.relay_stream_frames));
    json_object_object_add(
        root, "lan_backend_enabled",
        json_object_new_boolean(config.lan_backend_enabled));
    json_object_object_add(
        root, "remote_backend_enabled",
        json_object_new_boolean(config.remote_backend_enabled));
    json_object_object_add(
        root, "remote_backend_maps",
        json_object_new_uint64(config.remote_map_count));
    (void)agent_invoke_dynamic_map_prune(
        &dynamic_backend_maps, monotonic_ms());
    json_object_object_add(
        root, "automatic_https_agent_maps",
        json_object_new_uint64(dynamic_backend_maps.count));
    json_object_object_add(
        root, "automatic_https_agent_maps_accepted",
        json_object_new_uint64(dynamic_backend_maps.accepted));
    json_object_object_add(
        root, "automatic_https_agent_maps_renewed",
        json_object_new_uint64(dynamic_backend_maps.renewed));
    json_object_object_add(
        root, "automatic_https_agent_maps_removed",
        json_object_new_uint64(dynamic_backend_maps.removed));
    json_object_object_add(
        root, "automatic_https_agent_maps_expired",
        json_object_new_uint64(dynamic_backend_maps.expired));
    json_object_object_add(
        root, "trusted_backend_ca_bundle_id",
        json_object_new_string(config.remote_backend_ca_bundle_id));
    json_object_object_add(
        root, "remote_tls_handshakes",
        json_object_new_uint64(runtime.remote_tls_handshakes));
    json_object_object_add(
        root, "remote_tls_failures",
        json_object_new_uint64(runtime.remote_tls_failures));
    json_object_object_add(
        root, "retry_enabled",
        json_object_new_boolean(config.retry_enabled));
    json_object_object_add(
        root, "max_route_attempts",
        json_object_new_uint64(config.max_route_attempts));
    json_object_object_add(
        root, "retry_requests",
        json_object_new_uint64(runtime.retry_requests));
    json_object_object_add(
        root, "retry_attempts",
        json_object_new_uint64(runtime.retry_attempts));
    json_object_object_add(
        root, "retry_successes",
        json_object_new_uint64(runtime.retry_successes));
    json_object_object_add(
        root, "retry_exhausted",
        json_object_new_uint64(runtime.retry_exhausted));
    json_object_object_add(
        root, "retry_budget_rejections",
        json_object_new_uint64(runtime.retry_budget_rejections));
    json_object_object_add(
        root, "public_ingress_enabled",
        json_object_new_boolean(config.public_ingress_enabled));
    json_object_object_add(
        root, "public_ingress_requests",
        json_object_new_uint64(runtime.public_ingress_requests));
    json_object_object_add(
        root, "public_ingress_rejections",
        json_object_new_uint64(runtime.public_ingress_rejections));
    json_object_object_add(
        root, "public_ingress_route_misses",
        json_object_new_uint64(runtime.public_ingress_route_misses));
    json_object_object_add(
        root, "last_public_ipv6",
        runtime.last_public_ipv6[0] != '\0'
            ? json_object_new_string(runtime.last_public_ipv6)
            : json_object_new_null());
    json_object_object_add(
        root, "retry_attempt_timeout_ms",
        json_object_new_uint64(config.retry_attempt_timeout_ms));
    json_object_object_add(
        root, "retry_min_remaining_ms",
        json_object_new_uint64(config.retry_min_remaining_ms));
    json_object_object_add(root, "max_inflight",
                           json_object_new_uint64(config.max_inflight));
    json_object_object_add(root, "rate_per_second",
                           json_object_new_uint64(config.rate_per_second));
    json_object_object_add(root, "rate_burst",
                           json_object_new_uint64(config.rate_burst));
    json_object_object_add(
        root, "backend_timeout_ms",
        json_object_new_uint64(config.backend_timeout_ms));
    json_object_object_add(
        root, "max_backend_response_bytes",
        json_object_new_uint64(config.max_backend_response_bytes));
    json_object_object_add(
        root, "stream_idle_timeout_ms",
        json_object_new_uint64(config.stream_idle_timeout_ms));
    json_object_object_add(
        root, "max_stream_event_bytes",
        json_object_new_uint64(config.max_stream_event_bytes));
    send_json(connection, 200, root);
    json_object_put(root);
}

static bool add_required_scopes(
    struct json_object *root,
    const char *operation,
    const char *first,
    const char *second
)
{
    struct json_object *values = json_object_new_array();
    struct json_object *scope;

    if (values == NULL) return false;
    scope = json_object_new_string(first);
    if (scope == NULL || json_object_array_add(values, scope) != 0) {
        if (scope != NULL) json_object_put(scope);
        json_object_put(values);
        return false;
    }
    if (second != NULL) {
        scope = json_object_new_string(second);
        if (scope == NULL || json_object_array_add(values, scope) != 0) {
            if (scope != NULL) json_object_put(scope);
            json_object_put(values);
            return false;
        }
    }
    json_object_object_add(root, operation, values);
    return true;
}

static bool bootstrap_scopes_valid(struct json_object *root)
{
    struct json_object *scopes;
    struct json_object *scope;
    size_t index;
    size_t count;
    bool registration = false;

    if (!json_object_object_get_ex(root, "scopes", &scopes) ||
        !json_object_is_type(scopes, json_type_array)) return false;
    count = json_object_array_length(scopes);
    if (count == 0U || count > 3U) return false;
    for (index = 0U; index < count; index++) {
        const char *value;

        scope = json_object_array_get_idx(scopes, index);
        if (scope == NULL || !json_object_is_type(scope, json_type_string)) {
            return false;
        }
        value = json_object_get_string(scope);
        if (value == NULL ||
            (strcmp(value, "agent.register") != 0 &&
             strcmp(value, "agent.route") != 0 &&
             strcmp(value, "agent.invoke") != 0)) return false;
        if (strcmp(value, "agent.register") == 0) registration = true;
    }
    return registration;
}

static bool parse_bootstrap_request(
    const char *body,
    size_t body_length,
    char tenant[AGENT_IPC_TENANT_LEN],
    char origin[AGENT_IPC_URI_LEN]
)
{
    struct json_tokener *tokener = json_tokener_new_ex(8);
    struct json_object *root = NULL;
    char error[128];
    char prefix[AGENT_IPC_TENANT_LEN + 16U];
    int written;
    bool valid = false;

    if (tokener == NULL || body == NULL || body_length == 0U ||
        body_length > 4096U || body_length > (size_t)INT32_MAX) goto done;
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tokener, body, (int)body_length);
    if (root == NULL || json_tokener_get_error(tokener) != json_tokener_success ||
        json_tokener_get_parse_end(tokener) != body_length ||
        !json_object_is_type(root, json_type_object) ||
        !json_copy_required_string(root, "tenant", tenant,
                                   AGENT_IPC_TENANT_LEN, error,
                                   sizeof(error)) ||
        !json_copy_required_string(root, "origin", origin,
                                   AGENT_IPC_URI_LEN, error,
                                   sizeof(error)) ||
        !bootstrap_scopes_valid(root)) goto done;
    written = snprintf(prefix, sizeof(prefix), "agent://%s/", tenant);
    valid = written > 0 && (size_t)written < sizeof(prefix) &&
        strncmp(origin, prefix, (size_t)written) == 0 &&
        origin[written] != '\0';

done:
    if (root != NULL) json_object_put(root);
    if (tokener != NULL) json_tokener_free(tokener);
    return valid;
}

static bool random_nonce(
    uint8_t output[AGENT_LAN_SESSION_NONCE_LEN]
)
{
    size_t used = 0U;

    while (used < AGENT_LAN_SESSION_NONCE_LEN) {
        ssize_t result = getrandom(
            output + used, AGENT_LAN_SESSION_NONCE_LEN - used, 0U);
        if (result > 0) {
            used += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        memset(output, 0, AGENT_LAN_SESSION_NONCE_LEN);
        return false;
    }
    return true;
}

static void bootstrap_handler(struct uh_connection *connection, int event)
{
    struct json_object *root = NULL;
    struct json_object *identity = NULL;
    struct uh_str body;
    uint8_t nonce[AGENT_LAN_SESSION_NONCE_LEN];
    char source[AGENT_LAN_SESSION_SOURCE_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char origin[AGENT_IPC_URI_LEN];
    char token[AGENT_LAN_SESSION_TOKEN_MAX];
    enum agent_lan_session_result result;

    if (event == UH_EV_HEAD_COMPLETE) {
        release_request(connection);
        if (strcmp(connection->get_method_str(connection), "POST") != 0) {
            send_error(connection, 405, "METHOD_NOT_ALLOWED",
                       "use POST for this endpoint");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        if (!capture_trusted_lan_source(connection, source)) {
            runtime.lan_bootstrap_rejected++;
            audit_lan_bootstrap(
                "rejected", "untrusted", "bridge_authentication");
            send_error(connection, 404, "NOT_FOUND", "endpoint not found");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        if (connection->get_content_length(connection) == 0U ||
            connection->get_content_length(connection) > 4096U) {
            runtime.lan_bootstrap_rejected++;
            audit_lan_bootstrap("rejected", source, "content_length");
            send_error(connection, 400, "INVALID_BOOTSTRAP",
                       "bootstrap requires one bounded JSON object");
            connection->userdata = CONNECTION_REJECTED;
            return;
        }
        connection->check_expect_100_continue(connection);
        return;
    }
    if (event != UH_EV_COMPLETE) return;
    if (connection->userdata == CONNECTION_REJECTED) {
        connection->userdata = NULL;
        return;
    }
    if (!capture_trusted_lan_source(connection, source)) {
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap("rejected", "untrusted", "bridge_authentication");
        send_error(connection, 404, "NOT_FOUND", "endpoint not found");
        return;
    }
    if (!lan_source_rate_allow(source, monotonic_ms())) {
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap("rejected", source, "rate_limit");
        send_error(connection, 429, "BOOTSTRAP_RATE_LIMITED",
                   "LAN bootstrap rate exceeded");
        return;
    }
    body = connection->get_body(connection);
    if (!content_type_is_json(connection) || body.p == NULL ||
        body.len == 0U || body.len > 4096U) {
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap("rejected", source, "content_type_or_size");
        send_error(connection, 400, "INVALID_BOOTSTRAP",
                   "bootstrap requires one bounded JSON object");
        return;
    }
    if (!parse_bootstrap_request(body.p, body.len, tenant, origin)) {
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap("rejected", source, "identity_or_scopes");
        send_error(connection, 400, "INVALID_BOOTSTRAP",
                   "tenant, origin or scopes are invalid");
        return;
    }
    if (!random_nonce(nonce)) {
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap("rejected", source, "random_unavailable");
        send_error(connection, 500, "BOOTSTRAP_UNAVAILABLE",
                   "could not create a LAN session");
        return;
    }
    result = agent_lan_session_issue(
        config.lan_session_key, source, tenant, origin, wall_clock_seconds(),
        AGENT_LAN_SESSION_MAX_TTL_SECONDS, nonce, token, sizeof(token));
    memset(nonce, 0, sizeof(nonce));
    if (result != AGENT_LAN_SESSION_OK) {
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap(
            "rejected", source, agent_lan_session_result_name(result));
        send_error(connection, 500, "BOOTSTRAP_UNAVAILABLE",
                   "could not issue a LAN session");
        return;
    }
    root = json_object_new_object();
    identity = json_object_new_object();
    if (root == NULL || identity == NULL) {
        if (root != NULL) json_object_put(root);
        if (identity != NULL) json_object_put(identity);
        memset(token, 0, sizeof(token));
        runtime.lan_bootstrap_rejected++;
        audit_lan_bootstrap("rejected", source, "out_of_memory");
        send_error(connection, 500, "OUT_OF_MEMORY", "failed to build bootstrap response");
        return;
    }
    json_object_object_add(root, "schema_version", json_object_new_int(2));
    json_object_object_add(root, "access_token", json_object_new_string(token));
    json_object_object_add(root, "token_type", json_object_new_string("Bearer"));
    json_object_object_add(root, "expires_in",
                           json_object_new_int(AGENT_LAN_SESSION_MAX_TTL_SECONDS));
    json_object_object_add(root, "scope",
                           json_object_new_string(AGENT_LAN_SESSION_SCOPE));
    json_object_object_add(identity, "tenant", json_object_new_string(tenant));
    json_object_object_add(identity, "origin", json_object_new_string(origin));
    json_object_object_add(root, "identity", identity);
    if (config.cloud_trust_ready &&
        access(GATEWAY_CLOUD_ENROLLED_FILE, R_OK) == 0) {
        struct json_object *cloud_trust = json_object_new_object();
        if (cloud_trust != NULL) {
            json_object_object_add(
                cloud_trust, "mode",
                json_object_new_string(
                    config.cloud_trust_system ? "system" : "pinned-pem"));
            json_object_object_add(
                cloud_trust, "origin",
                json_object_new_string(config.cloud_public_origin));
            if (!config.cloud_trust_system) {
                json_object_object_add(
                    cloud_trust, "sha256",
                    json_object_new_string(config.cloud_trust_ca_sha256));
                json_object_object_add(
                    cloud_trust, "ca_pem",
                    json_object_new_string(config.cloud_trust_ca_pem));
            }
            json_object_object_add(root, "cloud_trust", cloud_trust);
        }
    }
    send_json(connection, 200, root);
    runtime.lan_bootstrap_issued++;
    if (lan_source_mark_bootstrap_issued(source)) {
        runtime.lan_bootstrap_refreshed++;
        audit_lan_bootstrap("issued", source, "refresh");
    } else {
        audit_lan_bootstrap("issued", source, "initial");
    }
    json_object_put(root);
    memset(token, 0, sizeof(token));
}

static void authentication_metadata_handler(
    struct uh_connection *connection,
    int event
)
{
    static const char metadata_path[] =
        "/agent/v1/authentication";
    struct json_object *root;
    struct json_object *authentication;
    struct json_object *required_scopes;
    struct uh_str path;
    char lan_source[AGENT_LAN_SESSION_SOURCE_LEN];
    bool trusted_lan;

    if (event != UH_EV_COMPLETE) return;
    path = connection->get_path(connection);
    if (path.len != sizeof(metadata_path) - 1U ||
        memcmp(path.p, metadata_path, sizeof(metadata_path) - 1U) != 0) {
        send_error(connection, 404, "NOT_FOUND", "endpoint not found");
        return;
    }
    if (strcmp(connection->get_method_str(connection), "GET") != 0) {
        send_error(connection, 405, "METHOD_NOT_ALLOWED",
                   "use GET for this endpoint");
        return;
    }
    trusted_lan = capture_trusted_lan_source(connection, lan_source);
    root = json_object_new_object();
    authentication = json_object_new_object();
    required_scopes = json_object_new_object();
    if (root == NULL || authentication == NULL || required_scopes == NULL) {
        if (root != NULL) json_object_put(root);
        if (authentication != NULL) json_object_put(authentication);
        if (required_scopes != NULL) json_object_put(required_scopes);
        send_error(connection, 500, "OUT_OF_MEMORY",
                   "failed to build authentication metadata");
        return;
    }
    if (!add_required_scopes(required_scopes, "route", "agent.route", NULL) ||
        !add_required_scopes(required_scopes, "invoke", "agent.route",
                             "agent.invoke") ||
        !add_required_scopes(required_scopes, "stream", "agent.route",
                             "agent.invoke") ||
        !add_required_scopes(required_scopes, "register", "agent.register",
                             NULL)) {
        json_object_put(root);
        json_object_put(authentication);
        json_object_put(required_scopes);
        send_error(connection, 500, "OUT_OF_MEMORY",
                   "failed to build authentication scopes");
        return;
    }
    json_object_object_add(root, "schema_version",
                           json_object_new_int(trusted_lan ? 2 : 1));
    json_object_object_add(authentication, "required",
                           json_object_new_boolean(config.jwt_required));
    json_object_object_add(authentication, "type",
                           json_object_new_string(
                               config.jwt_required ? "oauth2" : "none"));
    if (config.jwt_required) {
        json_object_object_add(authentication, "issuer",
                               json_object_new_string(config.jwt_issuer));
        json_object_object_add(authentication, "audience",
                               json_object_new_string(config.jwt_audience));
    }
    json_object_object_add(root, "authentication", authentication);
    json_object_object_add(root, "required_scopes", required_scopes);
    if (trusted_lan) {
        struct json_object *bootstrap = json_object_new_object();
        struct json_object *cloud = json_object_new_object();
        struct json_object *transports = json_object_new_object();
        char cloud_mode[16U] = {0};
        bool connector_enabled = read_cloud_transport_mode(cloud_mode);
        bool enrolled = access(GATEWAY_CLOUD_ENROLLED_FILE, R_OK) == 0;
        bool direct_available = config.public_descriptor_enabled &&
            (strcmp(cloud_mode, "direct_ipv6") == 0 ||
             strcmp(cloud_mode, "auto") == 0);
        bool relay_available = connector_enabled &&
            (strcmp(cloud_mode, "relay") == 0 ||
             strcmp(cloud_mode, "auto") == 0);
        if (bootstrap == NULL || cloud == NULL || transports == NULL) {
            if (bootstrap != NULL) json_object_put(bootstrap);
            if (cloud != NULL) json_object_put(cloud);
            if (transports != NULL) json_object_put(transports);
            json_object_put(root);
            send_error(connection, 500, "OUT_OF_MEMORY",
                       "failed to build LAN capability metadata");
            return;
        }
        json_object_object_add(bootstrap, "type",
                               json_object_new_string("trusted-lan"));
        json_object_object_add(bootstrap, "endpoint",
                               json_object_new_string("/agent/v1/bootstrap"));
        json_object_object_add(bootstrap, "token_ttl_seconds",
                               json_object_new_int(
                                   AGENT_LAN_SESSION_MAX_TTL_SECONDS));
        json_object_object_add(root, "bootstrap", bootstrap);
        json_object_object_add(cloud, "connector_enabled",
                               json_object_new_boolean(connector_enabled));
        json_object_object_add(cloud, "enrolled",
                               json_object_new_boolean(enrolled));
        json_object_object_add(cloud, "manifest_schema_version",
                               json_object_new_int(4));
        if (config.cloud_trust_ready && enrolled) {
            struct json_object *run_context_trust = json_object_new_object();
            if (run_context_trust != NULL) {
                json_object_object_add(
                    run_context_trust, "delivery",
                    json_object_new_string("bootstrap-response"));
                json_object_object_add(
                    cloud, "run_context_trust", run_context_trust);
            }
        }
        json_object_object_add(
            cloud, "status_endpoint",
            json_object_new_string("/agent/v1/cloud-registration"));
        json_object_object_add(
            transports, "direct_ipv6",
            json_object_new_boolean(direct_available));
        json_object_object_add(transports, "relay",
                               json_object_new_boolean(relay_available));
        json_object_object_add(
            transports, "auto",
            json_object_new_boolean(
                strcmp(cloud_mode, "auto") == 0 &&
                (direct_available || relay_available)));
        json_object_object_add(cloud, "transports", transports);
        json_object_object_add(root, "cloud", cloud);
    }
    send_json(connection, 200, root);
    json_object_put(root);
}

static void cloud_registration_response(
    struct uh_connection *connection,
    const char *state,
    const char *origin,
    const char *message
)
{
    struct json_object *root = json_object_new_object();

    if (root == NULL) {
        send_error(connection, 500, "OUT_OF_MEMORY",
                   "failed to build Cloud registration status");
        return;
    }
    json_object_object_add(root, "state", json_object_new_string(state));
    json_object_object_add(root, "origin", json_object_new_string(origin));
    json_object_object_add(root, "message", json_object_new_string(message));
    send_json(connection, 200, root);
    json_object_put(root);
}

static void cloud_registration_handler(
    struct uh_connection *connection,
    int event
)
{
    struct gateway_request_state *state;
    struct json_object *root = NULL;
    struct json_object *registrations;
    size_t index;
    char source[AGENT_LAN_SESSION_SOURCE_LEN];

    /* A bodyless GET is completed in one libuhttpd callback.  Allocating the
     * request during UH_EV_HEAD_COMPLETE makes the connection wait for a body
     * event which never arrives, so clients observe an empty disconnect. */
    if (event != UH_EV_COMPLETE) return;
    release_request(connection);
    if (strcmp(connection->get_method_str(connection), "GET") != 0) {
        send_error(connection, 405, "METHOD_NOT_ALLOWED",
                   "use GET for this endpoint");
        return;
    }
    if (!capture_trusted_lan_source(connection, source)) {
        send_error(connection, 404, "NOT_FOUND", "endpoint not found");
        return;
    }
    state = calloc(1U, sizeof(*state));
    if (state == NULL) {
        send_error(connection, 500, "OUT_OF_MEMORY",
                   "failed to allocate Cloud status state");
        return;
    }
    state->connection = connection;
    state->ipc_fd = -1;
    state->backend_fd = -1;
    connection->userdata = state;
    if (!authenticate_request(connection, state)) {
        send_error(connection, 401, "AUTHENTICATION_REQUIRED",
                   "a valid LAN Session token is required");
        goto done;
    }
    if (!state->lan_session) {
        send_error(connection, 404, "NOT_FOUND", "endpoint not found");
        goto done;
    }
    if (access(GATEWAY_CLOUD_ENABLED_FILE, R_OK) != 0) {
        cloud_registration_response(
            connection, "disabled", state->identity.source_agent,
            "Nexus Cloud connector is disabled on this router");
        goto done;
    }
    root = read_bounded_json_file(
        GATEWAY_CLOUD_STATUS_FILE, GATEWAY_CLOUD_STATUS_MAX);
    if (root == NULL ||
        !json_object_object_get_ex(root, "registrations", &registrations) ||
        !json_object_is_type(registrations, json_type_array)) {
        cloud_registration_response(
            connection, "pending", state->identity.source_agent,
            "Cloud connector has not completed its first reconciliation");
        goto done;
    }
    for (index = 0U; index < json_object_array_length(registrations); index++) {
        struct json_object *item =
            json_object_array_get_idx(registrations, index);
        struct json_object *origin;

        if (item == NULL || !json_object_is_type(item, json_type_object) ||
            !json_object_object_get_ex(item, "origin", &origin) ||
            !json_object_is_type(origin, json_type_string) ||
            strcmp(json_object_get_string(origin),
                   state->identity.source_agent) != 0) continue;
        send_json(connection, 200, item);
        goto done;
    }
    cloud_registration_response(
        connection, "pending", state->identity.source_agent,
        "Agent manifest is waiting for Cloud reconciliation");

done:
    if (root != NULL) json_object_put(root);
    release_request(connection);
}

static void not_found_handler(struct uh_connection *connection, int event)
{
    if (event == UH_EV_COMPLETE) {
        send_error(connection, 404, "NOT_FOUND", "endpoint not found");
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
            "Usage: %s [-a loopback:port] [-s ipc_socket] "
            "[-b max_envelope_bytes] [-t ipc_timeout_ms] "
            "[-c max_inflight] [-r rate_per_second] "
            "[-B rate_burst] [-j jwt_required] "
            "[-P jwt_public_key] [-K jwt_kid] [-J jwt_jwks_file] "
            "[-I jwt_issuer] [-A jwt_audience] "
            "[-S jwt_clock_skew] [-T jwt_max_lifetime] "
            "[-Q transaction_replay_capacity] "
            "[-f forwarding_assertion_enabled] "
            "[-p forwarding_private_key] [-k forwarding_kid] "
            "[-u forwarding_issuer] [-m forwarding_ttl_seconds] "
            "[-i invoke_enabled] [-g registration_enabled] "
            "[-e backend_timeout_ms] "
            "[-3 interactive_backend_timeout_ms] "
            "[-R max_backend_response_bytes] [-x stream_enabled] "
            "[-L stream_idle_timeout_ms] [-E max_stream_event_bytes] "
            "[-q stream_resume_enabled] [-w stream_resume_capacity] "
            "[-d stream_resume_ttl_seconds] "
            "[-M lan_backend_enabled] "
            "[-y remote_backend_enabled] [-F remote_ca_file] "
            "[-W trusted_remote_ca_bundle_id] "
            "[-n hostname:port=ipv4-or-[ipv6]]... [-z retry_enabled] "
            "[-N max_route_attempts] [-O retry_attempt_timeout_ms] "
            "[-G retry_min_remaining_ms] [-X public_ingress_enabled] "
            "[-Y public_ingress_token_file] "
            "[-D public_descriptor_enabled] "
            "[-V public_scheme] "
            "[-H public_tls_server_name] [-C public_ca_bundle_id] "
            "[-U public_ingress_port] [-o internal_invoke_enabled] "
            "[-l internal_invoke_token_digest_file] "
            "[-Z internal_invoke_generation] "
            "[-0 lan_bootstrap_enabled] [-1 lan_bridge_token_file] "
            "[-2 lan_session_key_file] [-4 cloud_public_origin] "
            "[-5 cloud_trust_ca_file] [-v]\n",
            program);
}

static bool read_options(int argc, char **argv)
{
    uint32_t value;
    size_t index;
    int option;

    snprintf(config.listen, sizeof(config.listen), "%s",
             GATEWAY_DEFAULT_LISTEN);
    snprintf(config.ipc_socket, sizeof(config.ipc_socket), "%s",
             GATEWAY_DEFAULT_IPC_SOCKET);
    config.max_envelope_bytes = GATEWAY_DEFAULT_MAX_ENVELOPE;
    config.ipc_timeout_ms = GATEWAY_DEFAULT_IPC_TIMEOUT_MS;
    config.max_inflight = GATEWAY_DEFAULT_MAX_INFLIGHT;
    config.rate_per_second = GATEWAY_DEFAULT_RATE_PER_SECOND;
    config.rate_burst = GATEWAY_DEFAULT_RATE_BURST;
    config.jwt_required = false;
    snprintf(config.jwt_public_key, sizeof(config.jwt_public_key), "%s",
             "/etc/agent-gw/jwt-public.pem");
    snprintf(config.jwt_key_id, sizeof(config.jwt_key_id), "%s", "default");
    config.jwt_jwks_file[0] = '\0';
    snprintf(config.jwt_issuer, sizeof(config.jwt_issuer), "%s",
             "https://issuer.invalid");
    snprintf(config.jwt_audience, sizeof(config.jwt_audience), "%s",
             "nexus-agent-router");
    config.jwt_clock_skew_seconds = 30U;
    config.jwt_max_lifetime_seconds = 300U;
    log_level(LOG_ERR);
    config.transaction_replay_capacity = 512U;
    config.forwarding_assertion_enabled = false;
    snprintf(config.forwarding_private_key,
             sizeof(config.forwarding_private_key), "%s",
             "/etc/agent-gw/forwarding-private.pem");
    snprintf(config.forwarding_key_id,
             sizeof(config.forwarding_key_id), "%s", "router-default");
    snprintf(config.forwarding_issuer,
             sizeof(config.forwarding_issuer), "%s", "router-local");
    config.forwarding_ttl_seconds = 30U;
    config.invoke_enabled = false;
    config.registration_enabled = false;
    config.backend_timeout_ms = GATEWAY_DEFAULT_BACKEND_TIMEOUT_MS;
    config.interactive_backend_timeout_ms =
        GATEWAY_DEFAULT_INTERACTIVE_BACKEND_TIMEOUT_MS;
    config.max_backend_response_bytes =
        GATEWAY_DEFAULT_MAX_BACKEND_RESPONSE;
    config.stream_enabled = false;
    config.stream_idle_timeout_ms = GATEWAY_DEFAULT_STREAM_IDLE_TIMEOUT_MS;
    config.max_stream_event_bytes = GATEWAY_DEFAULT_MAX_STREAM_EVENT;
    config.stream_resume_enabled = true;
    config.stream_resume_capacity = GATEWAY_DEFAULT_STREAM_RESUME_CAPACITY;
    config.stream_resume_ttl_seconds =
        GATEWAY_DEFAULT_STREAM_RESUME_TTL_SECONDS;
    config.lan_backend_enabled = true;
    config.remote_backend_enabled = true;
    snprintf(
        config.remote_backend_ca_file,
        sizeof(config.remote_backend_ca_file), "%s",
        "/etc/ssl/certs/ca-certificates.crt");
    snprintf(
        config.remote_backend_ca_bundle_id,
        sizeof(config.remote_backend_ca_bundle_id), "%s", "system");
    config.remote_map_count = 0U;
    config.retry_enabled = false;
    config.max_route_attempts = GATEWAY_DEFAULT_MAX_ROUTE_ATTEMPTS;
    config.retry_attempt_timeout_ms =
        GATEWAY_DEFAULT_RETRY_ATTEMPT_TIMEOUT_MS;
    config.retry_min_remaining_ms =
        GATEWAY_DEFAULT_RETRY_MIN_REMAINING_MS;
    config.public_ingress_enabled = false;
    snprintf(config.public_ingress_token_file,
             sizeof(config.public_ingress_token_file), "%s",
             "/var/run/agent-gw/public-ingress.token");
    config.public_ingress_token[0] = '\0';
    config.public_descriptor_enabled = false;
    snprintf(config.public_scheme, sizeof(config.public_scheme), "%s", "https");
    config.public_tls_server_name[0] = '\0';
    config.public_ca_bundle_id[0] = '\0';
    config.public_ingress_port = 7443U;
    config.internal_invoke_enabled = false;
    snprintf(config.internal_invoke_token_digest_file,
             sizeof(config.internal_invoke_token_digest_file), "%s",
             "/etc/agent-gw/nexus-cloud-gateway-token.sha256");
    config.internal_invoke_token_digest[0] = '\0';
    config.internal_invoke_generation[0] = '\0';
    config.lan_bootstrap_enabled = false;
    snprintf(config.lan_bridge_token_file,
             sizeof(config.lan_bridge_token_file), "%s",
             "/var/run/agent-gw/lan-bridge.token");
    snprintf(config.lan_session_key_file,
             sizeof(config.lan_session_key_file), "%s",
             "/var/run/agent-gw/lan-session.key");
    config.lan_bridge_token[0] = '\0';
    config.lan_session_key[0] = '\0';
    config.cloud_public_origin[0] = '\0';
    snprintf(config.cloud_trust_ca_file,
             sizeof(config.cloud_trust_ca_file), "%s",
             "/etc/ssl/certs/ca-certificates.crt");
    config.cloud_trust_ready = false;
    config.cloud_trust_system = false;
    config.cloud_trust_ca_pem[0] = '\0';
    config.cloud_trust_ca_sha256[0] = '\0';

    while ((option = getopt(
                argc, argv,
                "a:s:b:t:c:r:B:j:P:K:J:I:A:S:T:Q:f:p:k:u:m:i:g:e:3:R:x:L:E:q:w:d:M:y:F:W:n:z:N:O:G:X:Y:D:V:H:C:U:o:l:Z:0:1:2:4:5:vh")) != -1) {
        switch (option) {
        case 'a':
            if (!copy_text(config.listen, sizeof(config.listen), optarg)) {
                return false;
            }
            break;
        case 's':
            if (!copy_text(config.ipc_socket, sizeof(config.ipc_socket),
                           optarg)) {
                return false;
            }
            break;
        case 'b':
            if (!parse_u32(optarg, &value) ||
                value == 0U ||
                value > GATEWAY_MAX_ENVELOPE_LIMIT) {
                return false;
            }
            config.max_envelope_bytes = value;
            break;
        case 't':
            if (!parse_u32(optarg, &value) ||
                value < 10U ||
                value > 5000U) {
                return false;
            }
            config.ipc_timeout_ms = value;
            break;
        case 'c':
            if (!parse_u32(optarg, &value) ||
                value == 0U ||
                value > 1024U) {
                return false;
            }
            config.max_inflight = value;
            break;
        case 'r':
            if (!parse_u32(optarg, &value) ||
                value == 0U ||
                value > 100000U) {
                return false;
            }
            config.rate_per_second = value;
            break;
        case 'B':
            if (!parse_u32(optarg, &value) ||
                value == 0U ||
                value > 100000U) {
                return false;
            }
            config.rate_burst = value;
            break;
        case 'j':
            if (!parse_u32(optarg, &value) || value > 1U) {
                return false;
            }
            config.jwt_required = value == 1U;
            break;
        case 'P':
            if (!copy_text(config.jwt_public_key,
                           sizeof(config.jwt_public_key), optarg)) {
                return false;
            }
            break;
        case 'K':
            if (!copy_text(config.jwt_key_id,
                           sizeof(config.jwt_key_id), optarg)) {
                return false;
            }
            break;
        case 'J':
            if (!copy_text(config.jwt_jwks_file,
                           sizeof(config.jwt_jwks_file), optarg)) {
                return false;
            }
            break;
        case 'I':
            if (!copy_text(config.jwt_issuer,
                           sizeof(config.jwt_issuer), optarg)) {
                return false;
            }
            break;
        case 'A':
            if (!copy_text(config.jwt_audience,
                           sizeof(config.jwt_audience), optarg)) {
                return false;
            }
            break;
        case 'S':
            if (!parse_u32(optarg, &value) || value > 300U) {
                return false;
            }
            config.jwt_clock_skew_seconds = value;
            break;
        case 'T':
            if (!parse_u32(optarg, &value) ||
                value == 0U || value > 3600U) {
                return false;
            }
            config.jwt_max_lifetime_seconds = value;
            break;
        case 'Q':
            if (!parse_u32(optarg, &value) ||
                value == 0U ||
                value > AGENT_REPLAY_MAX_CAPACITY) {
                return false;
            }
            config.transaction_replay_capacity = value;
            break;
        case 'f':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.forwarding_assertion_enabled = value == 1U;
            break;
        case 'p':
            if (!copy_text(config.forwarding_private_key,
                           sizeof(config.forwarding_private_key), optarg)) {
                return false;
            }
            break;
        case 'k':
            if (!copy_text(config.forwarding_key_id,
                           sizeof(config.forwarding_key_id), optarg)) {
                return false;
            }
            break;
        case 'u':
            if (!copy_text(config.forwarding_issuer,
                           sizeof(config.forwarding_issuer), optarg)) {
                return false;
            }
            break;
        case 'm':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > AGENT_FORWARDING_MAX_TTL_SECONDS) return false;
            config.forwarding_ttl_seconds = value;
            break;
        case 'i':
            if (!parse_u32(optarg, &value) || value > 1U) {
                return false;
            }
            config.invoke_enabled = value == 1U;
            break;
        case 'g':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.registration_enabled = value == 1U;
            break;
        case 'e':
            if (!parse_u32(optarg, &value) ||
                value < 10U || value > 30000U) {
                return false;
            }
            config.backend_timeout_ms = value;
            break;
        case '3':
            if (!parse_u32(optarg, &value) || value < 1000U ||
                value > 3600000U) return false;
            config.interactive_backend_timeout_ms = value;
            break;
        case 'R':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > GATEWAY_MAX_BACKEND_RESPONSE_LIMIT) {
                return false;
            }
            config.max_backend_response_bytes = value;
            break;
        case 'x':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.stream_enabled = value == 1U;
            break;
        case 'L':
            if (!parse_u32(optarg, &value) ||
                value < 100U || value > 300000U) return false;
            config.stream_idle_timeout_ms = value;
            break;
        case 'E':
            if (!parse_u32(optarg, &value) || value < 64U ||
                value > GATEWAY_MAX_BACKEND_RESPONSE_LIMIT) return false;
            config.max_stream_event_bytes = value;
            break;
        case 'q':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.stream_resume_enabled = value == 1U;
            break;
        case 'w':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > AGENT_STREAM_RESUME_MAX_CAPACITY) return false;
            config.stream_resume_capacity = value;
            break;
        case 'd':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > 86400U) return false;
            config.stream_resume_ttl_seconds = value;
            break;
        case 'M':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.lan_backend_enabled = value == 1U;
            break;
        case 'y':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.remote_backend_enabled = value == 1U;
            break;
        case 'F':
            if (!copy_text(
                    config.remote_backend_ca_file,
                    sizeof(config.remote_backend_ca_file), optarg)) {
                return false;
            }
            break;
        case 'W':
            if (!copy_text(
                    config.remote_backend_ca_bundle_id,
                    sizeof(config.remote_backend_ca_bundle_id), optarg)) {
                return false;
            }
            break;
        case 'n':
            if (config.remote_map_count >= GATEWAY_MAX_REMOTE_MAPS ||
                !agent_invoke_parse_remote_map(
                    optarg,
                    &config.remote_maps[config.remote_map_count])) {
                return false;
            }
            for (index = 0U; index < config.remote_map_count; index++) {
                if (config.remote_maps[index].port ==
                        config.remote_maps[config.remote_map_count].port &&
                    strcmp(config.remote_maps[index].identity,
                           config.remote_maps[config.remote_map_count].identity) == 0) {
                    return false;
                }
            }
            config.remote_map_count++;
            break;
        case 'z':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.retry_enabled = value == 1U;
            break;
        case 'N':
            if (!parse_u32(optarg, &value) || value < 2U ||
                value > AGENT_IPC_MAX_CANDIDATES) return false;
            config.max_route_attempts = (uint8_t)value;
            break;
        case 'O':
            if (!parse_u32(optarg, &value) || value < 50U ||
                value > 30000U) return false;
            config.retry_attempt_timeout_ms = value;
            break;
        case 'G':
            if (!parse_u32(optarg, &value) || value < 10U ||
                value > 5000U) return false;
            config.retry_min_remaining_ms = value;
            break;
        case 'X':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.public_ingress_enabled = value == 1U;
            break;
        case 'Y':
            if (!copy_text(config.public_ingress_token_file,
                           sizeof(config.public_ingress_token_file),
                           optarg)) return false;
            break;
        case 'D':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.public_descriptor_enabled = value == 1U;
            break;
        case 'V':
            if (!copy_text(config.public_scheme,
                           sizeof(config.public_scheme), optarg) ||
                (strcmp(config.public_scheme, "https") != 0 &&
                 strcmp(config.public_scheme, "http") != 0)) return false;
            break;
        case 'H':
            if (!copy_text(config.public_tls_server_name,
                           sizeof(config.public_tls_server_name),
                           optarg)) return false;
            break;
        case 'C':
            if (!copy_text(config.public_ca_bundle_id,
                           sizeof(config.public_ca_bundle_id),
                           optarg)) return false;
            break;
        case 'U':
            if (!parse_u32(optarg, &value) || value == 0U ||
                value > 65535U) return false;
            config.public_ingress_port = (uint16_t)value;
            break;
        case 'o':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.internal_invoke_enabled = value == 1U;
            break;
        case 'l':
            if (!copy_text(config.internal_invoke_token_digest_file,
                           sizeof(config.internal_invoke_token_digest_file),
                           optarg)) return false;
            break;
        case 'Z':
            if (!copy_text(config.internal_invoke_generation,
                           sizeof(config.internal_invoke_generation),
                           optarg)) return false;
            break;
        case '0':
            if (!parse_u32(optarg, &value) || value > 1U) return false;
            config.lan_bootstrap_enabled = value == 1U;
            break;
        case '1':
            if (!copy_text(config.lan_bridge_token_file,
                           sizeof(config.lan_bridge_token_file),
                           optarg)) return false;
            break;
        case '2':
            if (!copy_text(config.lan_session_key_file,
                           sizeof(config.lan_session_key_file),
                           optarg)) return false;
            break;
        case '4':
            if (!copy_text(config.cloud_public_origin,
                           sizeof(config.cloud_public_origin),
                           optarg)) return false;
            break;
        case '5':
            if (!copy_text(config.cloud_trust_ca_file,
                           sizeof(config.cloud_trust_ca_file),
                           optarg)) return false;
            break;
        case 'v':
            log_level(LOG_INFO);
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            return false;
        }
    }

    return optind == argc && listen_is_loopback(config.listen) &&
           ipc_path_is_safe(config.ipc_socket) &&
           (!config.stream_enabled || config.invoke_enabled) &&
           (!config.retry_enabled || config.invoke_enabled) &&
           (!config.forwarding_assertion_enabled ||
            (config.jwt_required && config.invoke_enabled &&
             jwt_key_path_is_safe(config.forwarding_private_key) &&
             config.forwarding_key_id[0] != '\0' &&
             config.forwarding_issuer[0] != '\0')) &&
           (!config.retry_enabled ||
            config.retry_min_remaining_ms <= config.backend_timeout_ms) &&
           (!config.public_ingress_enabled ||
            (config.invoke_enabled &&
             strncmp(config.public_ingress_token_file,
                     "/var/run/agent-gw/", 18U) == 0 &&
             strstr(config.public_ingress_token_file, "..") == NULL)) &&
           (!config.public_descriptor_enabled ||
            (config.public_ingress_enabled &&
             (strcmp(config.public_scheme, "http") == 0 ||
              (descriptor_name_is_safe(
                   config.public_tls_server_name, false) &&
                descriptor_name_is_safe(
                    config.public_ca_bundle_id, true))))) &&
           (!config.internal_invoke_enabled ||
            (config.invoke_enabled && config.remote_backend_enabled &&
             internal_token_digest_path_is_safe(
                 config.internal_invoke_token_digest_file) &&
             agent_invoke_internal_generation_is_valid(
                 config.internal_invoke_generation))) &&
           (!config.lan_bootstrap_enabled ||
            (config.jwt_required && config.registration_enabled &&
             strcmp(config.lan_bridge_token_file,
                    "/var/run/agent-gw/lan-bridge.token") == 0 &&
             strcmp(config.lan_session_key_file,
                    "/var/run/agent-gw/lan-session.key") == 0)) &&
           (!config.stream_enabled ||
            config.max_stream_event_bytes <=
                config.max_backend_response_bytes) &&
           ((!config.remote_backend_enabled &&
             config.remote_map_count == 0U) ||
            (config.remote_backend_enabled &&
             backend_ca_path_is_safe(config.remote_backend_ca_file) &&
             descriptor_name_is_safe(
                 config.remote_backend_ca_bundle_id, true))) &&
           (!config.jwt_required ||
            ((((config.jwt_jwks_file[0] != '\0') &&
               jwt_key_path_is_safe(config.jwt_jwks_file)) ||
              ((config.jwt_jwks_file[0] == '\0') &&
               jwt_key_path_is_safe(config.jwt_public_key) &&
               config.jwt_key_id[0] != '\0')) &&
             config.jwt_issuer[0] != '\0' &&
             config.jwt_audience[0] != '\0'));
}

static bool initialize_authentication(void)
{
    struct agent_auth_policy policy;
    enum agent_jwt_result result;

    if (!config.jwt_required) {
        return true;
    }
    memset(&policy, 0, sizeof(policy));
    if (!copy_text(policy.issuer, sizeof(policy.issuer),
                   config.jwt_issuer) ||
        !copy_text(policy.audience, sizeof(policy.audience),
                   config.jwt_audience) ||
        !copy_text(policy.required_scope,
                   sizeof(policy.required_scope),
                   "agent.route")) {
        return false;
    }
    policy.clock_skew_seconds = config.jwt_clock_skew_seconds;
    policy.max_token_lifetime_seconds =
        config.jwt_max_lifetime_seconds;
    if (config.jwt_jwks_file[0] != '\0') {
        result = agent_jwt_verifier_init_jwks(
            &jwt_verifier, config.jwt_jwks_file, &policy);
    } else {
        result = agent_jwt_verifier_init(
            &jwt_verifier, config.jwt_public_key,
            config.jwt_key_id, &policy);
    }

    if (result != AGENT_JWT_OK) {
        fprintf(stderr,
                "agent-gw: JWT verifier initialization failed: %s\n",
                agent_jwt_result_name(result));
        return false;
    }
    if (!agent_replay_cache_init(
            &replay_cache,
            config.transaction_replay_capacity)) {
        fprintf(stderr,
                "agent-gw: invalid transaction replay cache\n");
        agent_jwt_verifier_free(&jwt_verifier);
        return false;
    }
    if (config.forwarding_assertion_enabled) {
        enum agent_forwarding_result forwarding_result =
            agent_forwarding_signer_init(
                &forwarding_signer, config.forwarding_private_key,
                config.forwarding_key_id, config.forwarding_issuer,
                config.forwarding_ttl_seconds);
        if (forwarding_result != AGENT_FORWARDING_OK) {
            fprintf(stderr,
                    "agent-gw: forwarding signer initialization failed: %s\n",
                    agent_forwarding_result_name(forwarding_result));
            agent_replay_cache_free(&replay_cache);
            agent_jwt_verifier_free(&jwt_verifier);
            return false;
        }
    }

    return true;
}
static bool initialize_remote_tls(void)
{
    if (!config.remote_backend_enabled) return true;
    if (!agent_tls_client_global_init(
            &tls_client, config.remote_backend_ca_file)) {
        fprintf(stderr,
                "agent-gw: remote backend TLS initialization failed\n");
        return false;
    }
    return true;
}

static bool initialize_stream_resume(void)
{
    if (!config.stream_resume_enabled) return true;
    if (!agent_stream_resume_cache_init(
            &stream_resume_cache, config.stream_resume_capacity,
            config.stream_resume_ttl_seconds)) {
        fprintf(stderr, "agent-gw: invalid stream resume cache\n");
        return false;
    }
    return true;
}

static void free_authentication(void)
{
    agent_stream_resume_cache_free(&stream_resume_cache);
    agent_forwarding_signer_free(&forwarding_signer);
    agent_tls_client_global_free(&tls_client);
    agent_replay_cache_free(&replay_cache);
    agent_jwt_verifier_free(&jwt_verifier);
}


int main(int argc, char **argv)
{
    struct ev_loop *loop = EV_DEFAULT;
    struct uh_server *server;
    ev_signal interrupt_watcher;
    ev_signal terminate_watcher;

    if (!read_options(argc, argv)) {
        usage(argv[0]);
        fprintf(stderr,
                "agent-gw: only loopback cleartext listeners are allowed\n");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    memset(&runtime, 0, sizeof(runtime));
    agent_invoke_dynamic_map_table_init(&dynamic_backend_maps);
    agent_tenant_quota_init(&runtime.tenant_quotas);
    if (!read_public_ingress_token()) {
        fprintf(stderr, "agent-gw: invalid public ingress token file\n");
        return EXIT_FAILURE;
    }
    if (!read_internal_invoke_token_digest()) {
        fprintf(stderr,
                "agent-gw: internal invoke verifier is unavailable or unsafe\n");
        return EXIT_FAILURE;
    }
    if (config.lan_bootstrap_enabled &&
        (!read_lan_secret(config.lan_bridge_token_file,
                          config.lan_bridge_token) ||
         !read_lan_secret(config.lan_session_key_file,
                          config.lan_session_key))) {
        fprintf(stderr, "agent-gw: LAN bootstrap secrets are unavailable or unsafe\n");
        return EXIT_FAILURE;
    }
    if (!read_cloud_trust()) {
        fprintf(stderr, "agent-gw: Cloud trust initialization failed\n");
        return EXIT_FAILURE;
    }
    if (!agent_rate_limiter_init(&runtime.rate_limiter,
                                 config.rate_per_second,
                                 config.rate_burst,
                                 monotonic_ms())) {
        fprintf(stderr, "agent-gw: invalid rate limiter configuration\n");
        return EXIT_FAILURE;
    }
    if (!initialize_authentication()) {
        return EXIT_FAILURE;
    }
    if (!initialize_stream_resume()) {
        free_authentication();
        return EXIT_FAILURE;
    }
    if (!initialize_remote_tls()) {
        free_authentication();
        return EXIT_FAILURE;
    }

    server = uh_server_new(loop);
    if (server == NULL) {
        fprintf(stderr, "agent-gw: failed to create HTTP server\n");
        free_authentication();
        return EXIT_FAILURE;
    }
    if (server->listen(server, config.listen, false) < 1) {
        fprintf(stderr, "agent-gw: failed to listen on %s\n", config.listen);
        server->free(server);
        free(server);
        free_authentication();
        return EXIT_FAILURE;
    }

    server->https_redirect(server, false);
    server->set_conn_closed_cb(server, connection_closed);
    server->set_default_handler(server, not_found_handler);
    if (server->add_path_handler(server, "^/healthz$", health_handler) != 0 ||
        server->add_path_handler(
            server, "^/agent/v1/authentication$",
            authentication_metadata_handler) != 0 ||
        server->add_path_handler(
            server, "^/agent/v1/bootstrap$", bootstrap_handler) != 0 ||
        server->add_path_handler(
            server, "^/agent/v1/cloud-registration$",
            cloud_registration_handler) != 0 ||
        server->add_path_handler(server, "^/agent/v1/route$", route_handler) != 0 ||
        server->add_path_handler(server, "^/agent/v1/invoke$", invoke_handler) != 0 ||
        server->add_path_handler(
            server, "^/agent/v1/internal-invoke$",
            internal_invoke_handler) != 0 ||
        server->add_path_handler(server, "^/agent/v1/invoke-stream$", stream_handler) != 0 ||
        server->add_path_handler(server, "^/agent/v1/register$", register_handler) != 0 ||
        server->add_path_handler(server, "^/agent/v1/renew$", renew_handler) != 0 ||
        server->add_path_handler(server, "^/agent/v1/unregister$", unregister_handler) != 0) {
        fprintf(stderr, "agent-gw: failed to register HTTP handlers\n");
        server->free(server);
        free(server);
        ev_loop_destroy(loop);
        free_authentication();
        return EXIT_FAILURE;
    }

    ev_signal_init(&interrupt_watcher, signal_callback, SIGINT);
    ev_signal_start(loop, &interrupt_watcher);
    ev_signal_init(&terminate_watcher, signal_callback, SIGTERM);
    ev_signal_start(loop, &terminate_watcher);
    ev_run(loop, 0);

    server->free(server);
    free(server);
    ev_loop_destroy(loop);
    free_authentication();
    return EXIT_SUCCESS;
}

#ifndef NEXUS_AGENT_RELAY_DIRECTORY_H
#define NEXUS_AGENT_RELAY_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_RELAY_DIRECTORY_ENDPOINT_LEN 256U
#define AGENT_RELAY_DIRECTORY_HOST_LEN 128U
#define AGENT_RELAY_DIRECTORY_AUTHORITY_LEN 192U
#define AGENT_RELAY_DIRECTORY_PATH_LEN 192U
#define AGENT_RELAY_DIRECTORY_PATH "/v1/relay-assignment"
#define AGENT_OPEN_MESH_DIRECTORY_PATH "/v1/open-mesh/assignment"
#define AGENT_RELAY_DIRECTORY_REQUEST_MAX 1024U
#define AGENT_RELAY_DIRECTORY_BODY_MAX 4096U
#define AGENT_RELAY_ASSIGNMENT_ID_LEN 65U
#define AGENT_RELAY_ID_LEN 65U
#define AGENT_RELAY_ROUTER_ID_LEN 65U
#define AGENT_RELAY_DOMAIN_ID_LEN 128U
#define AGENT_RELAY_ENDPOINT_LEN 256U
#define AGENT_RELAY_IPV4_LEN 16U
#define AGENT_RELAY_SESSION_TICKET_LEN 1537U
#define AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS 4U
#define AGENT_RELAY_MIN_LEASE_SECONDS 30U
#define AGENT_RELAY_MAX_LEASE_SECONDS 3600U

enum agent_relay_directory_result {
    AGENT_RELAY_DIRECTORY_OK = 0,
    AGENT_RELAY_DIRECTORY_INVALID = -1,
    AGENT_RELAY_DIRECTORY_UNSUPPORTED = -2,
    AGENT_RELAY_DIRECTORY_TOO_LARGE = -3,
    AGENT_RELAY_DIRECTORY_DUPLICATE = -4
};

struct agent_relay_directory_endpoint {
    char server_identity[AGENT_RELAY_DIRECTORY_HOST_LEN];
    char authority[AGENT_RELAY_DIRECTORY_AUTHORITY_LEN];
    char path[AGENT_RELAY_DIRECTORY_PATH_LEN];
    char connect_ipv4[AGENT_RELAY_IPV4_LEN];
    uint16_t port;
    bool open_mesh;
};

struct agent_relay_assignment {
    char assignment_id[AGENT_RELAY_ASSIGNMENT_ID_LEN];
    char relay_id[AGENT_RELAY_ID_LEN];
    char relay_router_id[AGENT_RELAY_ROUTER_ID_LEN];
    char relay_domain_id[AGENT_RELAY_DOMAIN_ID_LEN];
    char relay_endpoint[AGENT_RELAY_ENDPOINT_LEN];
    char connect_ipv4[AGENT_RELAY_IPV4_LEN];
    char session_ticket[AGENT_RELAY_SESSION_TICKET_LEN];
    uint32_t lease_seconds;
    uint64_t expires_at_ms;
    bool open_mesh;
    char mesh_connect_host[128];
    char mesh_tls_sha256[65];
};

bool agent_relay_directory_endpoint_parse(
    const char *endpoint,
    const char *connect_ipv4,
    struct agent_relay_directory_endpoint *parsed
);

bool agent_relay_directory_endpoint_set_parse(
    const char *endpoints,
    const char *connect_ipv4s,
    struct agent_relay_directory_endpoint *parsed,
    size_t parsed_capacity,
    size_t *parsed_count
);

enum agent_relay_directory_result agent_relay_directory_build_request(
    const struct agent_relay_directory_endpoint *endpoint,
    const char *router_id,
    const char *domain_id,
    char *request,
    size_t request_capacity,
    size_t *request_length
);

enum agent_relay_directory_result agent_relay_directory_build_ha_request(
    const struct agent_relay_directory_endpoint *endpoint,
    const char *router_id,
    const char *domain_id,
    const char *current_relay_id,
    const char *failed_relay_id,
    char *request,
    size_t request_capacity,
    size_t *request_length
);

enum agent_relay_directory_result
agent_relay_directory_build_authenticated_ha_request(
    const struct agent_relay_directory_endpoint *endpoint,
    const char *router_id,
    const char *domain_id,
    const char *current_relay_id,
    const char *failed_relay_id,
    const char *device_token,
    char *request,
    size_t request_capacity,
    size_t *request_length
);

enum agent_relay_directory_result agent_relay_directory_parse_assignment(
    const char *body,
    size_t body_length,
    uint64_t now_ms,
    struct agent_relay_assignment *assignment
);

bool agent_relay_assignment_equal(
    const struct agent_relay_assignment *left,
    const struct agent_relay_assignment *right
);

bool agent_relay_assignment_ticket_equal(
    const struct agent_relay_assignment *left,
    const struct agent_relay_assignment *right
);

#endif

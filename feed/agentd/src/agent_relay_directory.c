#include "agent_relay_directory.h"

#include "agent_peer_transport_contract.h"
#include "peer_table.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define FIELD_VERSION (1U << 0U)
#define FIELD_ASSIGNMENT_ID (1U << 1U)
#define FIELD_RELAY_ID (1U << 2U)
#define FIELD_RELAY_ROUTER_ID (1U << 3U)
#define FIELD_RELAY_DOMAIN_ID (1U << 4U)
#define FIELD_RELAY_ENDPOINT (1U << 5U)
#define FIELD_CONNECT_IPV4 (1U << 6U)
#define FIELD_LEASE_SECONDS (1U << 7U)
#define FIELD_SESSION_TICKET (1U << 8U)
#define REQUIRED_FIELDS ((1U << 9U) - 1U)

static bool host_valid(const char *host, size_t length)
{
    size_t label = 0U;
    size_t index;

    if (host == NULL || length == 0U ||
        length >= AGENT_RELAY_DIRECTORY_HOST_LEN) {
        return false;
    }
    for (index = 0U; index <= length; index++) {
        if (index == length || host[index] == '.') {
            if (index == label || index - label > 63U ||
                host[label] == '-' || host[index - 1U] == '-') {
                return false;
            }
            label = index + 1U;
        } else if (!islower((unsigned char)host[index]) &&
                   !isdigit((unsigned char)host[index]) &&
                   host[index] != '-') {
            return false;
        }
    }
    return true;
}

bool agent_relay_directory_endpoint_parse(
    const char *endpoint,
    const char *connect_ipv4,
    struct agent_relay_directory_endpoint *parsed
)
{
    static const char prefix[] = "https://";
    const char *authority;
    const char *slash;
    const char *colon;
    const char *cursor;
    size_t host_length;
    size_t authority_length;
    unsigned long port = 0UL;

    if (endpoint == NULL || connect_ipv4 == NULL || parsed == NULL ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0 ||
        (connect_ipv4[0] != '\0' &&
         !agent_peer_transport_ipv4_valid(connect_ipv4))) {
        return false;
    }
    authority = endpoint + sizeof(prefix) - 1U;
    slash = strchr(authority, '/');
    colon = slash == NULL ? NULL : memchr(authority, ':',
                                           (size_t)(slash - authority));
    if (slash == NULL ||
        (strcmp(slash, AGENT_RELAY_DIRECTORY_PATH) != 0 &&
         strcmp(slash, AGENT_OPEN_MESH_DIRECTORY_PATH) != 0 &&
         strcmp(slash, "/api/v1/edge/v1/relay-assignment/") != 0) ||
        (colon != NULL && (colon == authority || colon + 1 == slash))) {
        return false;
    }
    host_length = colon == NULL ? (size_t)(slash - authority)
                                : (size_t)(colon - authority);
    authority_length = (size_t)(slash - authority);
    if (!host_valid(authority, host_length) ||
        authority_length >= sizeof(parsed->authority) ||
        strlen(slash) >= sizeof(parsed->path)) {
        return false;
    }
    if (colon == NULL) {
        port = 443UL;
    } else {
        for (cursor = colon + 1; cursor < slash; cursor++) {
            if (!isdigit((unsigned char)*cursor) || port > 6553UL ||
                (port == 6553UL && (unsigned long)(*cursor - '0') > 5UL)) {
                return false;
            }
            port = (port * 10UL) + (unsigned long)(*cursor - '0');
        }
    }
    if (port == 0UL) {
        return false;
    }
    memset(parsed, 0, sizeof(*parsed));
    memcpy(parsed->server_identity, authority, host_length);
    memcpy(parsed->authority, authority, authority_length);
    memcpy(parsed->path, slash, strlen(slash) + 1U);
    if (snprintf(parsed->connect_ipv4, sizeof(parsed->connect_ipv4), "%s",
                 connect_ipv4) < 0) {
        memset(parsed, 0, sizeof(*parsed));
        return false;
    }
    parsed->port = (uint16_t)port;
    parsed->open_mesh = strcmp(slash, AGENT_OPEN_MESH_DIRECTORY_PATH) == 0;
    return true;
}

bool agent_relay_directory_endpoint_set_parse(
    const char *endpoints,
    const char *connect_ipv4s,
    struct agent_relay_directory_endpoint *parsed,
    size_t parsed_capacity,
    size_t *parsed_count
)
{
    const char *endpoint = endpoints;
    const char *address = connect_ipv4s;
    bool dns_mode;
    size_t count = 0U;

    if (parsed_count != NULL) *parsed_count = 0U;
    if (endpoints == NULL || connect_ipv4s == NULL || parsed == NULL ||
        parsed_count == NULL || parsed_capacity == 0U ||
        parsed_capacity > AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS ||
        endpoints[0] == '\0') {
        return false;
    }
    dns_mode = connect_ipv4s[0] == '\0';
    for (;;) {
        const char *endpoint_end = strchr(endpoint, ',');
        const char *address_end = dns_mode ? NULL : strchr(address, ',');
        size_t endpoint_length = endpoint_end == NULL
            ? strlen(endpoint) : (size_t)(endpoint_end - endpoint);
        size_t address_length = dns_mode ? 0U : (address_end == NULL
            ? strlen(address) : (size_t)(address_end - address));
        char endpoint_value[AGENT_RELAY_DIRECTORY_ENDPOINT_LEN];
        char address_value[AGENT_RELAY_IPV4_LEN];

        if ((!dns_mode &&
             (endpoint_end == NULL) != (address_end == NULL)) ||
            endpoint_length == 0U || (!dns_mode && address_length == 0U) ||
            endpoint_length >= sizeof(endpoint_value) ||
            address_length >= sizeof(address_value) ||
            count >= parsed_capacity) {
            return false;
        }
        memcpy(endpoint_value, endpoint, endpoint_length);
        endpoint_value[endpoint_length] = '\0';
        if (address_length > 0U) memcpy(address_value, address, address_length);
        address_value[address_length] = '\0';
        if (!agent_relay_directory_endpoint_parse(
                endpoint_value, address_value, &parsed[count])) {
            return false;
        }
        count++;
        if (endpoint_end == NULL) break;
        endpoint = endpoint_end + 1;
        if (!dns_mode) address = address_end + 1;
    }
    *parsed_count = count;
    return true;
}

enum agent_relay_directory_result agent_relay_directory_build_request(
    const struct agent_relay_directory_endpoint *endpoint,
    const char *router_id,
    const char *domain_id,
    char *request,
    size_t request_capacity,
    size_t *request_length
)
{
    return agent_relay_directory_build_ha_request(
        endpoint, router_id, domain_id, "", "", request,
        request_capacity, request_length);
}

enum agent_relay_directory_result agent_relay_directory_build_ha_request(
    const struct agent_relay_directory_endpoint *endpoint,
    const char *router_id,
    const char *domain_id,
    const char *current_relay_id,
    const char *failed_relay_id,
    char *request,
    size_t request_capacity,
    size_t *request_length
)
{
    return agent_relay_directory_build_authenticated_ha_request(
        endpoint, router_id, domain_id, current_relay_id, failed_relay_id,
        "", request, request_capacity, request_length);
}

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
)
{
    char body[640];
    char authorization[192] = {0};
    int body_size;
    int total;

    if (request_length != NULL) {
        *request_length = 0U;
    }
    if (endpoint == NULL || router_id == NULL || domain_id == NULL ||
        current_relay_id == NULL || failed_relay_id == NULL ||
        device_token == NULL ||
        request == NULL || request_capacity == 0U || request_length == NULL ||
        !agent_peer_id_valid(router_id, AGENT_RELAY_ROUTER_ID_LEN) ||
        !agent_peer_domain_valid(domain_id) ||
        (current_relay_id[0] != '\0' &&
         !agent_peer_id_valid(current_relay_id, AGENT_RELAY_ID_LEN)) ||
        (failed_relay_id[0] != '\0' &&
         !agent_peer_id_valid(failed_relay_id, AGENT_RELAY_ID_LEN))) {
        return AGENT_RELAY_DIRECTORY_INVALID;
    }
    if (device_token[0] != '\0') {
        size_t token_length = strnlen(device_token, 129U);
        size_t index;

        if (token_length < 6U || token_length > 128U ||
            strncmp(device_token, "edge_", 5U) != 0) {
            return AGENT_RELAY_DIRECTORY_INVALID;
        }
        for (index = 5U; index < token_length; index++) {
            if (!isalnum((unsigned char)device_token[index]) &&
                device_token[index] != '_' && device_token[index] != '-') {
                return AGENT_RELAY_DIRECTORY_INVALID;
            }
        }
        if (snprintf(authorization, sizeof(authorization),
                     "Authorization: Edge %s\r\n", device_token) < 0) {
            return AGENT_RELAY_DIRECTORY_TOO_LARGE;
        }
    }
    body_size = snprintf(body, sizeof(body),
                         "{\"version\":1,\"router_id\":\"%s\","
                         "\"domain_id\":\"%s\","
                         "\"current_relay_id\":\"%s\","
                         "\"failed_relay_id\":\"%s\"}",
                         router_id, domain_id, current_relay_id,
                         failed_relay_id);
    if (body_size < 0 || (size_t)body_size >= sizeof(body)) {
        return AGENT_RELAY_DIRECTORY_TOO_LARGE;
    }
    total = snprintf(
        request, request_capacity,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s"
        "Accept: application/vnd.nexus.relay-assignment+json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        endpoint->path, endpoint->authority, authorization, body_size, body);
    if (total < 0 || (size_t)total >= request_capacity) {
        return AGENT_RELAY_DIRECTORY_TOO_LARGE;
    }
    *request_length = (size_t)total;
    return AGENT_RELAY_DIRECTORY_OK;
}

static void skip_space(const char *body, size_t length, size_t *offset)
{
    while (*offset < length && isspace((unsigned char)body[*offset])) {
        (*offset)++;
    }
}

static bool consume(const char *body, size_t length, size_t *offset, char c)
{
    skip_space(body, length, offset);
    if (*offset >= length || body[*offset] != c) {
        return false;
    }
    (*offset)++;
    return true;
}

static bool json_string(
    const char *body,
    size_t length,
    size_t *offset,
    char *output,
    size_t capacity
)
{
    size_t used = 0U;

    if (!consume(body, length, offset, '"')) {
        return false;
    }
    while (*offset < length && body[*offset] != '"') {
        unsigned char value = (unsigned char)body[*offset];

        if (value < 0x20U || value > 0x7eU || value == '\\' ||
            used + 1U >= capacity) {
            return false;
        }
        output[used++] = (char)value;
        (*offset)++;
    }
    if (*offset >= length || body[*offset] != '"') {
        return false;
    }
    (*offset)++;
    output[used] = '\0';
    return used > 0U;
}

static bool json_u32(
    const char *body,
    size_t length,
    size_t *offset,
    uint32_t *value
)
{
    uint64_t parsed = 0U;
    size_t digits = 0U;

    skip_space(body, length, offset);
    while (*offset < length && isdigit((unsigned char)body[*offset])) {
        parsed = (parsed * 10U) + (uint64_t)(body[*offset] - '0');
        if (parsed > UINT32_MAX) {
            return false;
        }
        (*offset)++;
        digits++;
    }
    if (digits == 0U) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static uint32_t field_bit(const char *name)
{
    if (strcmp(name, "version") == 0) return FIELD_VERSION;
    if (strcmp(name, "assignment_id") == 0) return FIELD_ASSIGNMENT_ID;
    if (strcmp(name, "relay_id") == 0) return FIELD_RELAY_ID;
    if (strcmp(name, "relay_router_id") == 0) return FIELD_RELAY_ROUTER_ID;
    if (strcmp(name, "relay_domain_id") == 0) return FIELD_RELAY_DOMAIN_ID;
    if (strcmp(name, "relay_endpoint") == 0) return FIELD_RELAY_ENDPOINT;
    if (strcmp(name, "connect_ipv4") == 0) return FIELD_CONNECT_IPV4;
    if (strcmp(name, "lease_seconds") == 0) return FIELD_LEASE_SECONDS;
    if (strcmp(name, "session_ticket") == 0) return FIELD_SESSION_TICKET;
    return 0U;
}

static bool parse_field(
    uint32_t bit,
    const char *body,
    size_t length,
    size_t *offset,
    struct agent_relay_assignment *assignment,
    uint32_t *version
)
{
    if (bit == FIELD_VERSION) {
        return json_u32(body, length, offset, version);
    }
    if (bit == FIELD_LEASE_SECONDS) {
        return json_u32(body, length, offset, &assignment->lease_seconds);
    }
    if (bit == FIELD_ASSIGNMENT_ID) {
        return json_string(body, length, offset, assignment->assignment_id,
                           sizeof(assignment->assignment_id));
    }
    if (bit == FIELD_RELAY_ID) {
        return json_string(body, length, offset, assignment->relay_id,
                           sizeof(assignment->relay_id));
    }
    if (bit == FIELD_RELAY_ROUTER_ID) {
        return json_string(body, length, offset, assignment->relay_router_id,
                           sizeof(assignment->relay_router_id));
    }
    if (bit == FIELD_RELAY_DOMAIN_ID) {
        return json_string(body, length, offset, assignment->relay_domain_id,
                           sizeof(assignment->relay_domain_id));
    }
    if (bit == FIELD_RELAY_ENDPOINT) {
        return json_string(body, length, offset, assignment->relay_endpoint,
                           sizeof(assignment->relay_endpoint));
    }
    if (bit == FIELD_CONNECT_IPV4) {
        return json_string(body, length, offset, assignment->connect_ipv4,
                           sizeof(assignment->connect_ipv4));
    }
    if (bit == FIELD_SESSION_TICKET) {
        return json_string(body, length, offset, assignment->session_ticket,
                           sizeof(assignment->session_ticket));
    }
    return false;
}

static bool session_ticket_valid(const char *ticket)
{
    size_t index;
    size_t length;
    unsigned int dots = 0U;

    if (ticket == NULL) return false;
    length = strnlen(ticket, AGENT_RELAY_SESSION_TICKET_LEN);
    if (length < 64U || length >= AGENT_RELAY_SESSION_TICKET_LEN ||
        ticket[0] == '.' || ticket[length - 1U] == '.') {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char value = (unsigned char)ticket[index];

        if (value == '.') {
            if (index > 0U && ticket[index - 1U] == '.') return false;
            dots++;
        } else if (!isalnum(value) && value != '-' && value != '_') {
            return false;
        }
    }
    return dots == 3U && strncmp(ticket, "nrt1.", 5U) == 0;
}

static uint64_t saturating_expiry(uint64_t now_ms, uint32_t lease_seconds)
{
    uint64_t duration = (uint64_t)lease_seconds * 1000U;

    return UINT64_MAX - now_ms < duration ? UINT64_MAX : now_ms + duration;
}

enum agent_relay_directory_result agent_relay_directory_parse_assignment(
    const char *body,
    size_t body_length,
    uint64_t now_ms,
    struct agent_relay_assignment *assignment
)
{
    struct agent_relay_assignment parsed;
    struct agent_peer_transport_endpoint relay_endpoint;
    char key[32];
    uint32_t fields = 0U;
    uint32_t version = 0U;
    size_t offset = 0U;

    if (body == NULL || assignment == NULL || body_length == 0U) {
        return AGENT_RELAY_DIRECTORY_INVALID;
    }
    if (body_length > AGENT_RELAY_DIRECTORY_BODY_MAX) {
        return AGENT_RELAY_DIRECTORY_TOO_LARGE;
    }
    memset(&parsed, 0, sizeof(parsed));
    if (!consume(body, body_length, &offset, '{')) {
        return AGENT_RELAY_DIRECTORY_INVALID;
    }
    for (;;) {
        uint32_t bit;

        skip_space(body, body_length, &offset);
        if (offset < body_length && body[offset] == '}') {
            offset++;
            break;
        }
        if (!json_string(body, body_length, &offset, key, sizeof(key)) ||
            !consume(body, body_length, &offset, ':')) {
            return AGENT_RELAY_DIRECTORY_INVALID;
        }
        bit = field_bit(key);
        if (bit == 0U) {
            return AGENT_RELAY_DIRECTORY_UNSUPPORTED;
        }
        if ((fields & bit) != 0U) {
            return AGENT_RELAY_DIRECTORY_DUPLICATE;
        }
        if (!parse_field(bit, body, body_length, &offset, &parsed, &version)) {
            return AGENT_RELAY_DIRECTORY_INVALID;
        }
        fields |= bit;
        skip_space(body, body_length, &offset);
        if (offset < body_length && body[offset] == ',') {
            offset++;
            continue;
        }
        if (offset < body_length && body[offset] == '}') {
            offset++;
            break;
        }
        return AGENT_RELAY_DIRECTORY_INVALID;
    }
    skip_space(body, body_length, &offset);
    if (offset != body_length || fields != REQUIRED_FIELDS || version != 1U ||
        !agent_peer_id_valid(parsed.assignment_id,
                             sizeof(parsed.assignment_id)) ||
        !agent_peer_id_valid(parsed.relay_id, sizeof(parsed.relay_id)) ||
        !agent_peer_id_valid(parsed.relay_router_id,
                             sizeof(parsed.relay_router_id)) ||
        !agent_peer_domain_valid(parsed.relay_domain_id) ||
        !session_ticket_valid(parsed.session_ticket) ||
        parsed.lease_seconds < AGENT_RELAY_MIN_LEASE_SECONDS ||
        parsed.lease_seconds > AGENT_RELAY_MAX_LEASE_SECONDS ||
        !agent_peer_transport_endpoint_parse(parsed.relay_endpoint,
                                             parsed.connect_ipv4,
                                             &relay_endpoint)) {
        return AGENT_RELAY_DIRECTORY_INVALID;
    }
    parsed.expires_at_ms = saturating_expiry(now_ms, parsed.lease_seconds);
    *assignment = parsed;
    return AGENT_RELAY_DIRECTORY_OK;
}

bool agent_relay_assignment_equal(
    const struct agent_relay_assignment *left,
    const struct agent_relay_assignment *right
)
{
    return left != NULL && right != NULL &&
           left->open_mesh == right->open_mesh &&
           strcmp(left->assignment_id, right->assignment_id) == 0 &&
           strcmp(left->relay_id, right->relay_id) == 0 &&
           strcmp(left->relay_router_id, right->relay_router_id) == 0 &&
           strcmp(left->relay_domain_id, right->relay_domain_id) == 0 &&
           strcmp(left->relay_endpoint, right->relay_endpoint) == 0 &&
           strcmp(left->connect_ipv4, right->connect_ipv4) == 0;
}

bool agent_relay_assignment_ticket_equal(
    const struct agent_relay_assignment *left,
    const struct agent_relay_assignment *right
)
{
    return left != NULL && right != NULL &&
           strcmp(left->session_ticket, right->session_ticket) == 0;
}

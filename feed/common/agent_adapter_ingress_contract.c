#include "agent_adapter_ingress_contract.h"
#include "agent_public_ingress.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool component_character(unsigned char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-' ||
           character == ':' || character == '/' || character == '@';
}

static bool decode_component(
    const char *input,
    size_t input_length,
    char *output,
    size_t output_capacity
)
{
    size_t input_index = 0U;
    size_t output_index = 0U;

    if (input == NULL || output == NULL || input_length == 0U ||
        output_capacity < 2U) {
        return false;
    }
    while (input_index < input_length) {
        unsigned char character;

        if (input[input_index] == '%') {
            int high;
            int low;

            if (input_index + 2U >= input_length) {
                return false;
            }
            high = hex_value(input[input_index + 1U]);
            low = hex_value(input[input_index + 2U]);
            if (high < 0 || low < 0) {
                return false;
            }
            character = (unsigned char)((high << 4) | low);
            input_index += 3U;
        } else {
            character = (unsigned char)input[input_index++];
        }
        if (!component_character(character) ||
            output_index + 1U >= output_capacity) {
            return false;
        }
        output[output_index++] = (char)character;
    }
    output[output_index] = '\0';
    return output_index > 0U;
}

static const char *find_segment_end(const char *start, const char *end)
{
    const char *cursor;

    for (cursor = start; cursor < end; cursor++) {
        if (*cursor == '/') {
            return cursor;
        }
    }
    return end;
}

bool agent_adapter_ingress_parse_path(
    const char *path,
    size_t path_length,
    struct agent_adapter_ingress_route *route
)
{
    static const char mcp_prefix[] = "/mcp/";
    static const char a2a_prefix[] = "/a2a/";
    static const char a2a_send_suffix[] = "/message:send";
    static const char a2a_stream_suffix[] = "/message:stream";
    const char *end;
    const char *authority_start;
    const char *authority_end;
    const char *selector_start;
    const char *selector_end;

    if (path == NULL || route == NULL || path_length == 0U) {
        return false;
    }
    memset(route, 0, sizeof(*route));
    end = path + path_length;
    if (path_length > sizeof(mcp_prefix) - 1U &&
        memcmp(path, mcp_prefix, sizeof(mcp_prefix) - 1U) == 0) {
        authority_start = path + sizeof(mcp_prefix) - 1U;
        if (find_segment_end(authority_start, end) != end ||
            !decode_component(authority_start,
                              (size_t)(end - authority_start),
                              route->authority,
                              sizeof(route->authority))) {
            return false;
        }
        route->protocol = AGENT_ADAPTER_PROTOCOL_MCP;
        route->selector_from_request = true;
        return true;
    }
    if (path_length <= sizeof(a2a_prefix) - 1U ||
        memcmp(path, a2a_prefix, sizeof(a2a_prefix) - 1U) != 0) {
        return false;
    }
    authority_start = path + sizeof(a2a_prefix) - 1U;
    authority_end = find_segment_end(authority_start, end);
    if (authority_end == end) {
        return false;
    }
    selector_start = authority_end + 1U;
    selector_end = find_segment_end(selector_start, end);
    if (selector_end == end ||
        (!((size_t)(end - selector_end) == sizeof(a2a_send_suffix) - 1U &&
           memcmp(selector_end, a2a_send_suffix,
                  sizeof(a2a_send_suffix) - 1U) == 0) &&
         !((size_t)(end - selector_end) == sizeof(a2a_stream_suffix) - 1U &&
           memcmp(selector_end, a2a_stream_suffix,
                  sizeof(a2a_stream_suffix) - 1U) == 0)) ||
        !decode_component(authority_start,
                          (size_t)(authority_end - authority_start),
                          route->authority, sizeof(route->authority)) ||
        !decode_component(selector_start,
                          (size_t)(selector_end - selector_start),
                          route->selector, sizeof(route->selector))) {
        return false;
    }
    route->protocol = AGENT_ADAPTER_PROTOCOL_A2A;
    route->selector_from_request = false;
    route->streaming =
        (size_t)(end - selector_end) == sizeof(a2a_stream_suffix) - 1U;
    return true;
}

static bool header_value_safe(const char *value, size_t length)
{
    size_t index;

    if (value == NULL || length == 0U ||
        length > AGENT_ADAPTER_MAX_CREDENTIAL_LEN) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)value[index];

        if (character < 0x20U || character == 0x7fU) {
            return false;
        }
    }
    return true;
}

bool agent_adapter_build_gateway_request(
    const struct agent_invoke_endpoint *gateway,
    const struct agent_adapter_credentials *credentials,
    bool streaming,
    const char *envelope,
    size_t envelope_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    bool have_authorization;
    bool have_transaction;
    bool have_public_ipv6;
    bool have_edge_token;
    int written;
    size_t used;

    if (gateway == NULL || credentials == NULL || envelope == NULL ||
        envelope_length == 0U || output == NULL || output_capacity == 0U ||
        output_length == NULL || gateway->path[0] != '/') {
        return false;
    }
    have_authorization = credentials->authorization != NULL;
    have_transaction = credentials->transaction_token != NULL;
    have_public_ipv6 = credentials->public_ipv6 != NULL;
    have_edge_token = credentials->edge_token != NULL;
    if ((have_authorization && have_transaction) ||
        (have_authorization &&
         !header_value_safe(credentials->authorization,
                            credentials->authorization_length)) ||
        (have_transaction &&
         !header_value_safe(credentials->transaction_token,
                            credentials->transaction_token_length)) ||
        (have_public_ipv6 != have_edge_token) ||
        (have_public_ipv6 &&
         (!header_value_safe(credentials->public_ipv6,
                             credentials->public_ipv6_length) ||
          credentials->public_ipv6_length >= AGENT_PUBLIC_INGRESS_IPV6_MAX ||
          !header_value_safe(credentials->edge_token,
                             credentials->edge_token_length) ||
          credentials->edge_token_length >= AGENT_PUBLIC_INGRESS_TOKEN_MAX))) {
        return false;
    }
    if (gateway->family == AGENT_INVOKE_IPV6) {
        written = snprintf(
            output, output_capacity,
            "POST %s HTTP/1.1\r\nHost: [%s]:%u\r\n"
            "Content-Type: application/vnd.nexus.agent-envelope+json; charset=utf-8\r\n"
            "Accept: %s\r\nContent-Length: %" PRIu64
            "\r\nConnection: close\r\n",
            gateway->path, gateway->host, (unsigned int)gateway->port,
            streaming ? "text/event-stream" : "application/json",
            (uint64_t)envelope_length);
    } else {
        written = snprintf(
            output, output_capacity,
            "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
            "Content-Type: application/vnd.nexus.agent-envelope+json; charset=utf-8\r\n"
            "Accept: %s\r\nContent-Length: %" PRIu64
            "\r\nConnection: close\r\n",
            gateway->path, gateway->host, (unsigned int)gateway->port,
            streaming ? "text/event-stream" : "application/json",
            (uint64_t)envelope_length);
    }
    if (written < 0 || (size_t)written >= output_capacity) {
        return false;
    }
    used = (size_t)written;
    if (have_authorization) {
        written = snprintf(output + used, output_capacity - used,
                           "Authorization: %.*s\r\n",
                           (int)credentials->authorization_length,
                           credentials->authorization);
    } else if (have_transaction) {
        written = snprintf(output + used, output_capacity - used,
                           "Txn-Token: %.*s\r\n",
                           (int)credentials->transaction_token_length,
                           credentials->transaction_token);
    } else {
        written = 0;
    }
    if (written < 0 || (size_t)written >= output_capacity - used) {
        return false;
    }
    used += (size_t)written;
    if (have_public_ipv6) {
        written = snprintf(
            output + used, output_capacity - used,
            "%s: %.*s\r\n%s: %.*s\r\n",
            AGENT_PUBLIC_INGRESS_DEST_HEADER,
            (int)credentials->public_ipv6_length,
            credentials->public_ipv6,
            AGENT_PUBLIC_INGRESS_TOKEN_HEADER,
            (int)credentials->edge_token_length,
            credentials->edge_token);
        if (written < 0 || (size_t)written >= output_capacity - used) {
            return false;
        }
        used += (size_t)written;
    }
    if (output_capacity - used <= 2U ||
        envelope_length >= output_capacity - used - 2U) {
        return false;
    }
    memcpy(output + used, "\r\n", 2U);
    used += 2U;
    memcpy(output + used, envelope, envelope_length);
    used += envelope_length;
    output[used] = '\0';
    *output_length = used;
    return true;
}

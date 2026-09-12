#include "agent_edge_proxy.h"
#include "agent_public_ingress.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *find_crlf(const char *text, size_t length)
{
    size_t index;

    for (index = 0U; index + 1U < length; index++) {
        if (text[index] == '\r' && text[index + 1U] == '\n') {
            return text + index;
        }
    }
    return NULL;
}

static bool parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0' || !isdigit((unsigned char)text[0])) {
        return false;
    }
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0U || value > 65535U) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

bool agent_edge_proxy_parse_v1(
    const char *line,
    size_t line_length,
    struct agent_edge_proxy_info *info
)
{
    char copy[AGENT_EDGE_PROXY_LINE_MAX + 1U];
    char protocol[8];
    char source[46];
    char destination[46];
    char source_port[6];
    char destination_port[6];
    char extra;
    struct in6_addr parsed;
    int fields;

    if (line == NULL || info == NULL || line_length < 2U ||
        line_length > AGENT_EDGE_PROXY_LINE_MAX ||
        line[line_length - 2U] != '\r' || line[line_length - 1U] != '\n') {
        return false;
    }
    memcpy(copy, line, line_length - 2U);
    copy[line_length - 2U] = '\0';
    fields = sscanf(copy, "PROXY %7s %45s %45s %5s %5s %c",
                    protocol, source, destination, source_port,
                    destination_port, &extra);
    if (fields != 5 || strcmp(protocol, "TCP6") != 0 ||
        inet_pton(AF_INET6, source, &parsed) != 1 ||
        inet_pton(AF_INET6, destination, &parsed) != 1 ||
        !parse_port(source_port, &info->source_port) ||
        !parse_port(destination_port, &info->destination_port)) {
        return false;
    }
    if (inet_pton(AF_INET6, source, &parsed) != 1 ||
        inet_ntop(AF_INET6, &parsed, info->source_ipv6,
                  sizeof(info->source_ipv6)) == NULL ||
        inet_pton(AF_INET6, destination, &parsed) != 1 ||
        inet_ntop(AF_INET6, &parsed, info->destination_ipv6,
                  sizeof(info->destination_ipv6)) == NULL) return false;
    return true;
}

static bool header_named(const char *line, size_t length, const char *name)
{
    size_t name_length = strlen(name);

    return length > name_length && line[name_length] == ':' &&
           strncasecmp(line, name, name_length) == 0;
}

static enum agent_edge_backend public_request_backend(
    const char *line,
    size_t length
)
{
    const char *first_space;
    const char *second_space;
    size_t path_length;

    if (length < 14U ||
        (strncmp(line, "POST ", 5U) != 0 &&
         strncmp(line, "GET ", 4U) != 0)) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    first_space = strchr(line, ' ');
    if (first_space == NULL) return AGENT_EDGE_BACKEND_INVALID;
    second_space = memchr(first_space + 1U, ' ',
                          length - (size_t)(first_space + 1U - line));
    if (second_space == NULL ||
        (size_t)(line + length - second_space) != 9U ||
        memcmp(second_space, " HTTP/1.1", 9U) != 0) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    path_length = (size_t)(second_space - (first_space + 1U));
    if ((path_length == strlen("/agent/v1/invoke") &&
         memcmp(first_space + 1U, "/agent/v1/invoke", path_length) == 0) ||
        (path_length == strlen("/agent/v1/invoke-stream") &&
         memcmp(first_space + 1U, "/agent/v1/invoke-stream",
                path_length) == 0)) return AGENT_EDGE_BACKEND_GATEWAY;
    if (strncmp(line, "GET ", 4U) == 0 &&
        path_length == strlen("/agent/v1/authentication") &&
        memcmp(first_space + 1U, "/agent/v1/authentication",
               path_length) == 0) return AGENT_EDGE_BACKEND_GATEWAY;
    if ((path_length >= 5U &&
         memcmp(first_space + 1U, "/mcp/", 5U) == 0) ||
        (path_length >= 5U &&
         memcmp(first_space + 1U, "/a2a/", 5U) == 0)) {
        return AGENT_EDGE_BACKEND_ADAPTER;
    }
    return AGENT_EDGE_BACKEND_INVALID;
}

static bool exact_path(
    const char *path,
    size_t path_length,
    const char *expected
)
{
    return path_length == strlen(expected) &&
           memcmp(path, expected, path_length) == 0;
}

static enum agent_edge_backend lan_request_backend(
    const char *line,
    size_t length
)
{
    const char *first_space;
    const char *second_space;
    const char *path;
    size_t path_length;
    bool post;

    if (length < 14U ||
        (strncmp(line, "POST ", 5U) != 0 &&
         strncmp(line, "GET ", 4U) != 0)) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    post = strncmp(line, "POST ", 5U) == 0;
    first_space = strchr(line, ' ');
    if (first_space == NULL) return AGENT_EDGE_BACKEND_INVALID;
    second_space = memchr(first_space + 1U, ' ',
                          length - (size_t)(first_space + 1U - line));
    if (second_space == NULL ||
        (size_t)(line + length - second_space) != 9U ||
        memcmp(second_space, " HTTP/1.1", 9U) != 0) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    path = first_space + 1U;
    path_length = (size_t)(second_space - path);
    if (!post &&
        (exact_path(path, path_length, "/agent/v1/authentication") ||
         exact_path(path, path_length, "/agent/v1/cloud-registration"))) {
        return AGENT_EDGE_BACKEND_GATEWAY;
    }
    if (!post) return AGENT_EDGE_BACKEND_INVALID;
    if (exact_path(path, path_length, "/agent/v1/bootstrap") ||
        exact_path(path, path_length, "/agent/v1/route") ||
        exact_path(path, path_length, "/agent/v1/invoke") ||
        exact_path(path, path_length, "/agent/v1/invoke-stream") ||
        exact_path(path, path_length, "/agent/v1/register") ||
        exact_path(path, path_length, "/agent/v1/renew") ||
        exact_path(path, path_length, "/agent/v1/unregister")) {
        return AGENT_EDGE_BACKEND_GATEWAY;
    }
    if ((path_length >= 5U && memcmp(path, "/mcp/", 5U) == 0) ||
        (path_length >= 5U && memcmp(path, "/a2a/", 5U) == 0)) {
        return AGENT_EDGE_BACKEND_ADAPTER;
    }
    return AGENT_EDGE_BACKEND_INVALID;
}

static enum agent_edge_backend rewrite_http(
    const char *input,
    size_t input_length,
    const char *destination_ipv6,
    const char *edge_token,
    bool public_ingress,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    const char *line;
    const char *line_end;
    const char *header_end;
    enum agent_edge_backend backend;
    size_t used = 0U;
    int written;

    if (input == NULL || input_length < 4U ||
        input_length > AGENT_EDGE_HTTP_HEADER_MAX || output == NULL ||
        output_length == NULL ||
        (public_ingress && (destination_ipv6 == NULL || edge_token == NULL))) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    header_end = NULL;
    for (size_t index = 0U; index + 3U < input_length; index++) {
        if (memcmp(input + index, "\r\n\r\n", 4U) == 0) {
            header_end = input + index + 4U;
            break;
        }
    }
    if (header_end == NULL || header_end != input + input_length) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    line_end = find_crlf(input, input_length);
    if (line_end == NULL) return AGENT_EDGE_BACKEND_INVALID;
    backend = public_ingress
        ? public_request_backend(input, (size_t)(line_end - input))
        : lan_request_backend(input, (size_t)(line_end - input));
    if (backend == AGENT_EDGE_BACKEND_INVALID) return backend;
    if ((size_t)(line_end + 2U - input) >= output_capacity) return backend;
    memcpy(output, input, (size_t)(line_end + 2U - input));
    used = (size_t)(line_end + 2U - input);
    line = line_end + 2U;
    while (line < header_end - 2U) {
        size_t length;

        line_end = find_crlf(line, (size_t)(header_end - line));
        if (line_end == NULL) return AGENT_EDGE_BACKEND_INVALID;
        length = (size_t)(line_end - line);
        if (length == 0U) break;
        if (!header_named(line, length, AGENT_PUBLIC_INGRESS_DEST_HEADER) &&
            !header_named(line, length, AGENT_PUBLIC_INGRESS_TOKEN_HEADER) &&
            !header_named(line, length, AGENT_LAN_SOURCE_HEADER) &&
            !header_named(line, length, AGENT_LAN_BRIDGE_TOKEN_HEADER) &&
            !header_named(line, length, "Connection")) {
            if (length + 2U >= output_capacity - used) {
                return AGENT_EDGE_BACKEND_INVALID;
            }
            memcpy(output + used, line, length + 2U);
            used += length + 2U;
        }
        line = line_end + 2U;
    }
    if (public_ingress) {
        written = snprintf(
            output + used, output_capacity - used,
            "%s: %s\r\n%s: %s\r\nConnection: close\r\n\r\n",
            AGENT_PUBLIC_INGRESS_DEST_HEADER, destination_ipv6,
            AGENT_PUBLIC_INGRESS_TOKEN_HEADER, edge_token);
    } else if (destination_ipv6 != NULL && edge_token != NULL) {
        written = snprintf(
            output + used, output_capacity - used,
            "%s: %s\r\n%s: %s\r\nConnection: close\r\n\r\n",
            AGENT_LAN_SOURCE_HEADER, destination_ipv6,
            AGENT_LAN_BRIDGE_TOKEN_HEADER, edge_token);
    } else {
        written = snprintf(output + used, output_capacity - used,
                           "Connection: close\r\n\r\n");
    }
    if (written < 0 || (size_t)written >= output_capacity - used) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    *output_length = used + (size_t)written;
    return backend;
}

enum agent_edge_backend agent_edge_proxy_rewrite_http(
    const char *input,
    size_t input_length,
    const char *destination_ipv6,
    const char *edge_token,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    return rewrite_http(input, input_length, destination_ipv6, edge_token,
                        true, output, output_capacity, output_length);
}

enum agent_edge_backend agent_edge_proxy_rewrite_lan_http(
    const char *input,
    size_t input_length,
    const char *source_address,
    const char *bridge_token,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if ((source_address == NULL) != (bridge_token == NULL)) {
        return AGENT_EDGE_BACKEND_INVALID;
    }
    return rewrite_http(input, input_length, source_address, bridge_token,
                        false, output,
                        output_capacity, output_length);
}

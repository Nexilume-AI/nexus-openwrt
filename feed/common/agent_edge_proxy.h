#ifndef NEXUS_AGENT_EDGE_PROXY_H
#define NEXUS_AGENT_EDGE_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_EDGE_PROXY_LINE_MAX 108U
#define AGENT_EDGE_HTTP_HEADER_MAX 16384U
#define AGENT_LAN_SOURCE_HEADER "X-Nexus-Lan-Source"
#define AGENT_LAN_BRIDGE_TOKEN_HEADER "X-Nexus-Lan-Bridge-Token"

struct agent_edge_proxy_info {
    char source_ipv6[46];
    char destination_ipv6[46];
    uint16_t source_port;
    uint16_t destination_port;
};

enum agent_edge_backend {
    AGENT_EDGE_BACKEND_INVALID = 0,
    AGENT_EDGE_BACKEND_GATEWAY,
    AGENT_EDGE_BACKEND_ADAPTER
};

bool agent_edge_proxy_parse_v1(
    const char *line,
    size_t line_length,
    struct agent_edge_proxy_info *info
);

enum agent_edge_backend agent_edge_proxy_rewrite_http(
    const char *input,
    size_t input_length,
    const char *destination_ipv6,
    const char *edge_token,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

enum agent_edge_backend agent_edge_proxy_rewrite_lan_http(
    const char *input,
    size_t input_length,
    const char *source_address,
    const char *bridge_token,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

#endif

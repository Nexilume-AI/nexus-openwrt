#ifndef NEXUS_AGENT_ADAPTER_INGRESS_CONTRACT_H
#define NEXUS_AGENT_ADAPTER_INGRESS_CONTRACT_H

#include "agent_adapter_contract.h"
#include "agent_invoke_contract.h"

#include <stdbool.h>
#include <stddef.h>

#define AGENT_ADAPTER_MAX_CREDENTIAL_LEN 8192U

struct agent_adapter_ingress_route {
    enum agent_adapter_protocol protocol;
    char authority[AGENT_ADAPTER_AUTHORITY_LEN];
    char selector[AGENT_ADAPTER_SELECTOR_LEN];
    bool selector_from_request;
    bool streaming;
};

struct agent_adapter_credentials {
    const char *authorization;
    size_t authorization_length;
    const char *transaction_token;
    size_t transaction_token_length;
    const char *public_ipv6;
    size_t public_ipv6_length;
    const char *edge_token;
    size_t edge_token_length;
};

bool agent_adapter_ingress_parse_path(
    const char *path,
    size_t path_length,
    struct agent_adapter_ingress_route *route
);

bool agent_adapter_build_gateway_request(
    const struct agent_invoke_endpoint *gateway,
    const struct agent_adapter_credentials *credentials,
    bool streaming,
    const char *envelope,
    size_t envelope_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
);

#endif

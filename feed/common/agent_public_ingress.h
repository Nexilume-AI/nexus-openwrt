#ifndef NEXUS_AGENT_PUBLIC_INGRESS_H
#define NEXUS_AGENT_PUBLIC_INGRESS_H

#include <stddef.h>

#define AGENT_PUBLIC_INGRESS_DEST_HEADER "X-Nexus-Public-IPv6"
#define AGENT_PUBLIC_INGRESS_TOKEN_HEADER "X-Nexus-Edge-Token"
#define AGENT_PUBLIC_INGRESS_TOKEN_MAX 129U
#define AGENT_PUBLIC_INGRESS_IPV6_MAX 46U

enum agent_public_ingress_result {
    AGENT_PUBLIC_INGRESS_NONE = 0,
    AGENT_PUBLIC_INGRESS_TRUSTED = 1,
    AGENT_PUBLIC_INGRESS_INVALID = -1
};

enum agent_public_ingress_result agent_public_ingress_validate(
    const char *destination,
    size_t destination_length,
    const char *token,
    size_t token_length,
    const char *expected_token,
    char canonical_ipv6[AGENT_PUBLIC_INGRESS_IPV6_MAX]
);

#endif

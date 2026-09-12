#include "agent_public_ingress.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool token_equal(
    const char *left,
    size_t left_length,
    const char *right,
    size_t right_length
)
{
    size_t index;
    uint8_t difference = (uint8_t)(left_length ^ right_length);
    size_t maximum = left_length > right_length ? left_length : right_length;

    for (index = 0U; index < maximum; index++) {
        uint8_t a = index < left_length ? (uint8_t)left[index] : 0U;
        uint8_t b = index < right_length ? (uint8_t)right[index] : 0U;
        difference |= (uint8_t)(a ^ b);
    }
    return difference == 0U;
}

enum agent_public_ingress_result agent_public_ingress_validate(
    const char *destination,
    size_t destination_length,
    const char *token,
    size_t token_length,
    const char *expected_token,
    char canonical_ipv6[AGENT_PUBLIC_INGRESS_IPV6_MAX]
)
{
    char destination_text[AGENT_PUBLIC_INGRESS_IPV6_MAX];
    struct in6_addr parsed;
    size_t expected_length;

    if (canonical_ipv6 == NULL || expected_token == NULL) {
        return AGENT_PUBLIC_INGRESS_INVALID;
    }
    canonical_ipv6[0] = '\0';
    if (destination == NULL && token == NULL) return AGENT_PUBLIC_INGRESS_NONE;
    if (destination == NULL || token == NULL || destination_length == 0U ||
        destination_length >= sizeof(destination_text) || token_length == 0U ||
        token_length >= AGENT_PUBLIC_INGRESS_TOKEN_MAX) {
        return AGENT_PUBLIC_INGRESS_INVALID;
    }
    expected_length = strlen(expected_token);
    if (expected_length == 0U ||
        !token_equal(token, token_length, expected_token, expected_length)) {
        return AGENT_PUBLIC_INGRESS_INVALID;
    }
    memcpy(destination_text, destination, destination_length);
    destination_text[destination_length] = '\0';
    if (inet_pton(AF_INET6, destination_text, &parsed) != 1 ||
        inet_ntop(AF_INET6, &parsed, canonical_ipv6,
                  AGENT_PUBLIC_INGRESS_IPV6_MAX) == NULL) {
        canonical_ipv6[0] = '\0';
        return AGENT_PUBLIC_INGRESS_INVALID;
    }
    return AGENT_PUBLIC_INGRESS_TRUSTED;
}

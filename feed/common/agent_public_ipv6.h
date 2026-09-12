#ifndef NEXUS_AGENT_PUBLIC_IPV6_H
#define NEXUS_AGENT_PUBLIC_IPV6_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_PUBLIC_IPV6_TEXT_LEN 46U
#define AGENT_PUBLIC_IPV6_ROUTE_ID_LEN 33U
#define AGENT_PUBLIC_IPV6_ORIGIN_LEN 256U

enum agent_public_ipv6_result {
    AGENT_PUBLIC_IPV6_OK = 0,
    AGENT_PUBLIC_IPV6_INVALID = -1,
    AGENT_PUBLIC_IPV6_FULL = -2,
    AGENT_PUBLIC_IPV6_NOT_FOUND = -3,
    AGENT_PUBLIC_IPV6_COLLISION = -4
};

struct agent_public_ipv6_lease {
    char route_id[AGENT_PUBLIC_IPV6_ROUTE_ID_LEN];
    char origin[AGENT_PUBLIC_IPV6_ORIGIN_LEN];
    char address[AGENT_PUBLIC_IPV6_TEXT_LEN];
    uint64_t expires_ms;
};

struct agent_public_ipv6_pool {
    bool configured;
    uint8_t prefix[16];
    uint8_t prefix_length;
    char prefix_text[AGENT_PUBLIC_IPV6_TEXT_LEN + 4U];
    struct agent_public_ipv6_lease *leases;
    size_t capacity;
    size_t count;
};

typedef bool (*agent_public_ipv6_release_cb)(
    const struct agent_public_ipv6_lease *lease,
    void *context
);

bool agent_public_ipv6_pool_init(
    struct agent_public_ipv6_pool *pool,
    const char *prefix,
    size_t capacity
);

void agent_public_ipv6_pool_free(struct agent_public_ipv6_pool *pool);

enum agent_public_ipv6_result agent_public_ipv6_allocate(
    struct agent_public_ipv6_pool *pool,
    const char *route_id,
    const char *origin,
    uint64_t expires_ms,
    struct agent_public_ipv6_lease *lease
);

enum agent_public_ipv6_result agent_public_ipv6_renew(
    struct agent_public_ipv6_pool *pool,
    const char *route_id,
    const char *origin,
    uint64_t expires_ms,
    struct agent_public_ipv6_lease *lease
);

enum agent_public_ipv6_result agent_public_ipv6_release(
    struct agent_public_ipv6_pool *pool,
    const char *route_id,
    struct agent_public_ipv6_lease *lease
);

const struct agent_public_ipv6_lease *agent_public_ipv6_find(
    const struct agent_public_ipv6_pool *pool,
    const char *route_id
);

const struct agent_public_ipv6_lease *agent_public_ipv6_find_address(
    const struct agent_public_ipv6_pool *pool,
    const char *address
);

size_t agent_public_ipv6_prune(
    struct agent_public_ipv6_pool *pool,
    uint64_t now_ms,
    agent_public_ipv6_release_cb release_cb,
    void *context
);

bool agent_public_ipv6_contains(
    const struct agent_public_ipv6_pool *pool,
    const char *address
);

#endif

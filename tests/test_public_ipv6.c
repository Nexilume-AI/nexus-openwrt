#include "agent_public_ipv6.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct release_state {
    size_t calls;
    bool accept;
};

static bool release_address(
    const struct agent_public_ipv6_lease *lease,
    void *context
)
{
    struct release_state *state = context;
    assert(lease != NULL);
    state->calls++;
    return state->accept;
}

int main(void)
{
    struct agent_public_ipv6_pool pool;
    struct agent_public_ipv6_lease first;
    struct agent_public_ipv6_lease renewed;
    struct release_state release = {0U, false};

    assert(!agent_public_ipv6_pool_init(&pool, "2001:db8::/47", 2U));
    assert(!agent_public_ipv6_pool_init(&pool, "2001:db8::/65", 2U));
    assert(agent_public_ipv6_pool_init(&pool, "2001:db8:1234:5678::/64", 2U));
    assert(strcmp(pool.prefix_text, "2001:db8:1234:5678::/64") == 0);
    assert(agent_public_ipv6_allocate(
        &pool, "00000000000000000000000000000001", "agent://a", 1000U,
        &first) == AGENT_PUBLIC_IPV6_OK);
    assert(strcmp(first.address, "2001:db8:1234:5678::1") == 0);
    assert(agent_public_ipv6_contains(&pool, first.address));
    assert(!agent_public_ipv6_contains(&pool, "2001:db8:1234:5679::1"));

    assert(agent_public_ipv6_allocate(
        &pool, first.route_id, "agent://a", 2000U,
        &renewed) == AGENT_PUBLIC_IPV6_OK);
    assert(strcmp(first.address, renewed.address) == 0);
    assert(agent_public_ipv6_find_address(&pool, first.address) != NULL);
    assert(strcmp(agent_public_ipv6_find_address(&pool, first.address)->route_id,
                  first.route_id) == 0);
    assert(agent_public_ipv6_find_address(
               &pool, "2001:db8:1234:56ff::1") == NULL);
    assert(renewed.expires_ms == 2000U);
    assert(pool.count == 1U);
    assert(agent_public_ipv6_renew(
        &pool, first.route_id, "agent://other", 3000U, NULL) ==
        AGENT_PUBLIC_IPV6_COLLISION);

    assert(agent_public_ipv6_allocate(
        &pool, "11111111111111112222222222222222", "agent://b", 500U,
        NULL) == AGENT_PUBLIC_IPV6_OK);
    assert(agent_public_ipv6_allocate(
        &pool, "33333333333333334444444444444444", "agent://c", 500U,
        NULL) == AGENT_PUBLIC_IPV6_FULL);

    assert(agent_public_ipv6_prune(&pool, 600U, release_address, &release) == 0U);
    assert(release.calls == 1U);
    assert(pool.count == 2U);
    release.accept = true;
    assert(agent_public_ipv6_prune(&pool, 600U, release_address, &release) == 1U);
    assert(pool.count == 1U);
    assert(agent_public_ipv6_release(&pool, first.route_id, NULL) ==
           AGENT_PUBLIC_IPV6_OK);
    assert(agent_public_ipv6_release(&pool, first.route_id, NULL) ==
           AGENT_PUBLIC_IPV6_NOT_FOUND);
    agent_public_ipv6_pool_free(&pool);
    puts("public IPv6 allocator tests passed");
    return 0;
}

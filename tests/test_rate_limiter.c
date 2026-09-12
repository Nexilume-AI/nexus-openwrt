#include "agent_rate_limiter.h"

#include <assert.h>
#include <stdio.h>

static void test_invalid_configuration(void)
{
    struct agent_rate_limiter limiter;

    assert(!agent_rate_limiter_init(NULL, 1U, 1U, 0U));
    assert(!agent_rate_limiter_init(&limiter, 0U, 1U, 0U));
    assert(!agent_rate_limiter_init(&limiter, 1U, 0U, 0U));
}

static void test_burst_and_refill(void)
{
    struct agent_rate_limiter limiter;

    assert(agent_rate_limiter_init(&limiter, 2U, 3U, 1000U));
    assert(agent_rate_limiter_allow(&limiter, 1000U));
    assert(agent_rate_limiter_allow(&limiter, 1000U));
    assert(agent_rate_limiter_allow(&limiter, 1000U));
    assert(!agent_rate_limiter_allow(&limiter, 1000U));
    assert(!agent_rate_limiter_allow(&limiter, 1499U));
    assert(agent_rate_limiter_allow(&limiter, 1500U));
    assert(!agent_rate_limiter_allow(&limiter, 1500U));
    assert(agent_rate_limiter_allow(&limiter, 3000U));
    assert(agent_rate_limiter_allow(&limiter, 3000U));
    assert(agent_rate_limiter_allow(&limiter, 3000U));
    assert(!agent_rate_limiter_allow(&limiter, 3000U));
}

static void test_clock_rollback_does_not_refill(void)
{
    struct agent_rate_limiter limiter;

    assert(agent_rate_limiter_init(&limiter, 1U, 1U, 5000U));
    assert(agent_rate_limiter_allow(&limiter, 5000U));
    assert(!agent_rate_limiter_allow(&limiter, 4000U));
    assert(agent_rate_limiter_allow(&limiter, 6000U));
}

int main(void)
{
    test_invalid_configuration();
    test_burst_and_refill();
    test_clock_rollback_does_not_refill();
    puts("agent rate limiter tests passed");
    return 0;
}

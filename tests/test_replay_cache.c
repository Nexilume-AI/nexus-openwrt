#include "agent_replay_cache.h"

#include <assert.h>
#include <stdio.h>

static void test_replay_and_expiry(void)
{
    struct agent_replay_cache cache;

    assert(agent_replay_cache_init(&cache, 2U));
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer-a", "txn-1", 120U, 100U) ==
           AGENT_REPLAY_ACCEPTED);
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer-a", "txn-1", 120U, 101U) ==
           AGENT_REPLAY_DETECTED);
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer-b", "txn-1", 120U, 101U) ==
           AGENT_REPLAY_ACCEPTED);
    assert(cache.used == 2U);
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer-a", "txn-2", 130U, 101U) ==
           AGENT_REPLAY_CAPACITY_EXCEEDED);
    assert(agent_replay_cache_prune(&cache, 120U) == 2U);
    assert(cache.used == 0U);
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer-a", "txn-1", 140U, 120U) ==
           AGENT_REPLAY_ACCEPTED);
    agent_replay_cache_free(&cache);
}

static void test_invalid_inputs(void)
{
    struct agent_replay_cache cache;

    assert(!agent_replay_cache_init(&cache, 0U));
    assert(!agent_replay_cache_init(
        &cache, AGENT_REPLAY_MAX_CAPACITY + 1U));
    assert(agent_replay_cache_init(&cache, 1U));
    assert(agent_replay_cache_check_and_store(
               &cache, "", "txn", 2U, 1U) ==
           AGENT_REPLAY_INVALID_ARGUMENT);
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer", "", 2U, 1U) ==
           AGENT_REPLAY_INVALID_ARGUMENT);
    assert(agent_replay_cache_check_and_store(
               &cache, "issuer", "txn", 1U, 1U) ==
           AGENT_REPLAY_INVALID_ARGUMENT);
    agent_replay_cache_free(&cache);
}

int main(void)
{
    test_replay_and_expiry();
    test_invalid_inputs();
    puts("agent replay cache tests passed");
    return 0;
}

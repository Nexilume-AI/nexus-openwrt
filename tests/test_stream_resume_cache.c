#include "agent_stream_resume_cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct agent_stream_resume_cache cache;
    char route_id[AGENT_IPC_ROUTE_ID_LEN];

    assert(agent_stream_resume_cache_init(&cache, 2U, 5U));
    assert(agent_stream_resume_cache_store(
        &cache, "task-1", "tenant-a", "agent://caller", "demo.stream",
        "0123456789abcdef0123456789abcdef", 1000U) ==
        AGENT_STREAM_RESUME_OK);
    assert(agent_stream_resume_cache_lookup(
        &cache, "task-1", "tenant-a", "agent://caller", "demo.stream",
        2000U, route_id) == AGENT_STREAM_RESUME_OK);
    assert(strcmp(route_id, "0123456789abcdef0123456789abcdef") == 0);
    assert(agent_stream_resume_cache_lookup(
        &cache, "task-1", "tenant-b", "agent://caller", "demo.stream",
        2000U, route_id) == AGENT_STREAM_RESUME_NOT_FOUND);
    assert(agent_stream_resume_cache_store(
        &cache, "task-2", "tenant-a", "agent://caller", "demo.stream",
        "1123456789abcdef0123456789abcdef", 2000U) ==
        AGENT_STREAM_RESUME_OK);
    assert(agent_stream_resume_cache_store(
        &cache, "task-3", "tenant-a", "agent://caller", "demo.stream",
        "2123456789abcdef0123456789abcdef", 2000U) ==
        AGENT_STREAM_RESUME_CAPACITY);
    assert(agent_stream_resume_cache_prune(&cache, 8001U) == 2U);
    assert(agent_stream_resume_cache_store(
        &cache, "task-3", "tenant-a", "agent://caller", "demo.stream",
        "2123456789abcdef0123456789abcdef", 8001U) ==
        AGENT_STREAM_RESUME_OK);
    agent_stream_resume_cache_free(&cache);
    puts("stream resume cache tests passed");
    return 0;
}

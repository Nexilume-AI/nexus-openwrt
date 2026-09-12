#ifndef NEXUS_AGENT_STREAM_RESUME_CACHE_H
#define NEXUS_AGENT_STREAM_RESUME_CACHE_H

#include "agent_ipc_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_STREAM_RESUME_MAX_CAPACITY 4096U

struct agent_stream_resume_entry {
    bool used;
    uint64_t expires_at_ms;
    char task_id[AGENT_IPC_TASK_ID_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char source_agent[AGENT_IPC_AGENT_ID_LEN];
    char intent[AGENT_IPC_INTENT_LEN];
    char route_id[AGENT_IPC_ROUTE_ID_LEN];
};

struct agent_stream_resume_cache {
    struct agent_stream_resume_entry *entries;
    size_t capacity;
    uint64_t ttl_ms;
};

enum agent_stream_resume_result {
    AGENT_STREAM_RESUME_OK = 0,
    AGENT_STREAM_RESUME_NOT_FOUND,
    AGENT_STREAM_RESUME_CAPACITY,
    AGENT_STREAM_RESUME_INVALID
};

bool agent_stream_resume_cache_init(
    struct agent_stream_resume_cache *cache,
    size_t capacity,
    uint32_t ttl_seconds
);

void agent_stream_resume_cache_free(struct agent_stream_resume_cache *cache);

size_t agent_stream_resume_cache_prune(
    struct agent_stream_resume_cache *cache,
    uint64_t now_ms
);

enum agent_stream_resume_result agent_stream_resume_cache_store(
    struct agent_stream_resume_cache *cache,
    const char *task_id,
    const char *tenant,
    const char *source_agent,
    const char *intent,
    const char *route_id,
    uint64_t now_ms
);

enum agent_stream_resume_result agent_stream_resume_cache_lookup(
    struct agent_stream_resume_cache *cache,
    const char *task_id,
    const char *tenant,
    const char *source_agent,
    const char *intent,
    uint64_t now_ms,
    char route_id[AGENT_IPC_ROUTE_ID_LEN]
);

#endif

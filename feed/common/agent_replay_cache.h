#ifndef NEXUS_AGENT_REPLAY_CACHE_H
#define NEXUS_AGENT_REPLAY_CACHE_H

#include "agent_auth_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_REPLAY_MAX_CAPACITY 4096U

struct agent_replay_entry {
    char issuer[AGENT_AUTH_ISSUER_LEN];
    char transaction_id[AGENT_AUTH_TXN_LEN];
    uint64_t expires_at;
    bool occupied;
};

struct agent_replay_cache {
    struct agent_replay_entry *entries;
    size_t capacity;
    size_t used;
};

enum agent_replay_result {
    AGENT_REPLAY_ACCEPTED = 0,
    AGENT_REPLAY_DETECTED,
    AGENT_REPLAY_CAPACITY_EXCEEDED,
    AGENT_REPLAY_INVALID_ARGUMENT
};

bool agent_replay_cache_init(
    struct agent_replay_cache *cache,
    size_t capacity
);

void agent_replay_cache_free(struct agent_replay_cache *cache);

enum agent_replay_result agent_replay_cache_check_and_store(
    struct agent_replay_cache *cache,
    const char *issuer,
    const char *transaction_id,
    uint64_t expires_at,
    uint64_t now_seconds
);

size_t agent_replay_cache_prune(
    struct agent_replay_cache *cache,
    uint64_t now_seconds
);

const char *agent_replay_result_name(enum agent_replay_result result);

#endif

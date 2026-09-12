#include "agent_replay_cache.h"

#include <stdlib.h>
#include <string.h>

static bool text_fits(const char *text, size_t capacity)
{
    size_t length;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    length = strlen(text);
    return length < capacity;
}

bool agent_replay_cache_init(
    struct agent_replay_cache *cache,
    size_t capacity
)
{
    if (cache == NULL || capacity == 0U ||
        capacity > AGENT_REPLAY_MAX_CAPACITY) {
        return false;
    }
    memset(cache, 0, sizeof(*cache));
    cache->entries = calloc(capacity, sizeof(*cache->entries));
    if (cache->entries == NULL) {
        return false;
    }
    cache->capacity = capacity;
    return true;
}

void agent_replay_cache_free(struct agent_replay_cache *cache)
{
    if (cache == NULL) {
        return;
    }
    free(cache->entries);
    memset(cache, 0, sizeof(*cache));
}

size_t agent_replay_cache_prune(
    struct agent_replay_cache *cache,
    uint64_t now_seconds
)
{
    size_t index;
    size_t pruned = 0U;

    if (cache == NULL || cache->entries == NULL) {
        return 0U;
    }
    for (index = 0U; index < cache->capacity; index++) {
        if (cache->entries[index].occupied &&
            cache->entries[index].expires_at <= now_seconds) {
            memset(&cache->entries[index], 0,
                   sizeof(cache->entries[index]));
            cache->used--;
            pruned++;
        }
    }
    return pruned;
}

enum agent_replay_result agent_replay_cache_check_and_store(
    struct agent_replay_cache *cache,
    const char *issuer,
    const char *transaction_id,
    uint64_t expires_at,
    uint64_t now_seconds
)
{
    struct agent_replay_entry *free_entry = NULL;
    size_t index;

    if (cache == NULL || cache->entries == NULL ||
        !text_fits(issuer, AGENT_AUTH_ISSUER_LEN) ||
        !text_fits(transaction_id, AGENT_AUTH_TXN_LEN) ||
        expires_at <= now_seconds) {
        return AGENT_REPLAY_INVALID_ARGUMENT;
    }

    (void)agent_replay_cache_prune(cache, now_seconds);
    for (index = 0U; index < cache->capacity; index++) {
        if (!cache->entries[index].occupied) {
            if (free_entry == NULL) {
                free_entry = &cache->entries[index];
            }
            continue;
        }
        if (strcmp(cache->entries[index].issuer, issuer) == 0 &&
            strcmp(cache->entries[index].transaction_id,
                   transaction_id) == 0) {
            return AGENT_REPLAY_DETECTED;
        }
    }
    if (free_entry == NULL) {
        return AGENT_REPLAY_CAPACITY_EXCEEDED;
    }

    memcpy(free_entry->issuer, issuer, strlen(issuer) + 1U);
    memcpy(free_entry->transaction_id, transaction_id,
           strlen(transaction_id) + 1U);
    free_entry->expires_at = expires_at;
    free_entry->occupied = true;
    cache->used++;
    return AGENT_REPLAY_ACCEPTED;
}

const char *agent_replay_result_name(enum agent_replay_result result)
{
    switch (result) {
    case AGENT_REPLAY_ACCEPTED:
        return "accepted";
    case AGENT_REPLAY_DETECTED:
        return "replay_detected";
    case AGENT_REPLAY_CAPACITY_EXCEEDED:
        return "capacity_exceeded";
    case AGENT_REPLAY_INVALID_ARGUMENT:
        return "invalid_argument";
    default:
        return "unknown";
    }
}

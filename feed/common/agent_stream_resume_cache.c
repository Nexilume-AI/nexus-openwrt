#include "agent_stream_resume_cache.h"

#include <stdlib.h>
#include <string.h>

static bool bounded_text(const char *value, size_t capacity)
{
    size_t length;

    if (value == NULL || value[0] == '\0') return false;
    length = strnlen(value, capacity);
    return length > 0U && length < capacity;
}

static bool key_matches(
    const struct agent_stream_resume_entry *entry,
    const char *task_id,
    const char *tenant,
    const char *source_agent,
    const char *intent
)
{
    return entry->used &&
           strcmp(entry->task_id, task_id) == 0 &&
           strcmp(entry->tenant, tenant) == 0 &&
           strcmp(entry->source_agent, source_agent) == 0 &&
           strcmp(entry->intent, intent) == 0;
}

bool agent_stream_resume_cache_init(
    struct agent_stream_resume_cache *cache,
    size_t capacity,
    uint32_t ttl_seconds
)
{
    if (cache == NULL || capacity == 0U ||
        capacity > AGENT_STREAM_RESUME_MAX_CAPACITY ||
        ttl_seconds == 0U || ttl_seconds > 86400U) return false;
    memset(cache, 0, sizeof(*cache));
    cache->entries = calloc(capacity, sizeof(*cache->entries));
    if (cache->entries == NULL) return false;
    cache->capacity = capacity;
    cache->ttl_ms = (uint64_t)ttl_seconds * 1000U;
    return true;
}

void agent_stream_resume_cache_free(struct agent_stream_resume_cache *cache)
{
    if (cache == NULL) return;
    free(cache->entries);
    memset(cache, 0, sizeof(*cache));
}

size_t agent_stream_resume_cache_prune(
    struct agent_stream_resume_cache *cache,
    uint64_t now_ms
)
{
    size_t index;
    size_t removed = 0U;

    if (cache == NULL || cache->entries == NULL) return 0U;
    for (index = 0U; index < cache->capacity; index++) {
        if (cache->entries[index].used &&
            cache->entries[index].expires_at_ms <= now_ms) {
            memset(&cache->entries[index], 0, sizeof(cache->entries[index]));
            removed++;
        }
    }
    return removed;
}

enum agent_stream_resume_result agent_stream_resume_cache_store(
    struct agent_stream_resume_cache *cache,
    const char *task_id,
    const char *tenant,
    const char *source_agent,
    const char *intent,
    const char *route_id,
    uint64_t now_ms
)
{
    size_t index;
    struct agent_stream_resume_entry *slot = NULL;

    if (cache == NULL || cache->entries == NULL ||
        !bounded_text(task_id, AGENT_IPC_TASK_ID_LEN) ||
        !bounded_text(tenant, AGENT_IPC_TENANT_LEN) ||
        !bounded_text(source_agent, AGENT_IPC_AGENT_ID_LEN) ||
        !bounded_text(intent, AGENT_IPC_INTENT_LEN) ||
        !bounded_text(route_id, AGENT_IPC_ROUTE_ID_LEN) ||
        now_ms > UINT64_MAX - cache->ttl_ms) {
        return AGENT_STREAM_RESUME_INVALID;
    }
    (void)agent_stream_resume_cache_prune(cache, now_ms);
    for (index = 0U; index < cache->capacity; index++) {
        if (key_matches(&cache->entries[index], task_id, tenant,
                        source_agent, intent)) {
            slot = &cache->entries[index];
            break;
        }
        if (!cache->entries[index].used && slot == NULL) {
            slot = &cache->entries[index];
        }
    }
    if (slot == NULL) return AGENT_STREAM_RESUME_CAPACITY;
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->expires_at_ms = now_ms + cache->ttl_ms;
    (void)strcpy(slot->task_id, task_id);
    (void)strcpy(slot->tenant, tenant);
    (void)strcpy(slot->source_agent, source_agent);
    (void)strcpy(slot->intent, intent);
    (void)strcpy(slot->route_id, route_id);
    return AGENT_STREAM_RESUME_OK;
}

enum agent_stream_resume_result agent_stream_resume_cache_lookup(
    struct agent_stream_resume_cache *cache,
    const char *task_id,
    const char *tenant,
    const char *source_agent,
    const char *intent,
    uint64_t now_ms,
    char route_id[AGENT_IPC_ROUTE_ID_LEN]
)
{
    size_t index;

    if (cache == NULL || cache->entries == NULL || route_id == NULL ||
        !bounded_text(task_id, AGENT_IPC_TASK_ID_LEN) ||
        !bounded_text(tenant, AGENT_IPC_TENANT_LEN) ||
        !bounded_text(source_agent, AGENT_IPC_AGENT_ID_LEN) ||
        !bounded_text(intent, AGENT_IPC_INTENT_LEN)) {
        return AGENT_STREAM_RESUME_INVALID;
    }
    route_id[0] = '\0';
    (void)agent_stream_resume_cache_prune(cache, now_ms);
    for (index = 0U; index < cache->capacity; index++) {
        if (key_matches(&cache->entries[index], task_id, tenant,
                        source_agent, intent)) {
            (void)strcpy(route_id, cache->entries[index].route_id);
            return AGENT_STREAM_RESUME_OK;
        }
    }
    return AGENT_STREAM_RESUME_NOT_FOUND;
}

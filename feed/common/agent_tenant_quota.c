#include "agent_tenant_quota.h"

#include <stdio.h>
#include <string.h>

void agent_tenant_quota_init(struct agent_tenant_quota_table *table)
{
    if (table != NULL) memset(table, 0, sizeof(*table));
}

static struct agent_tenant_quota_slot *find_slot(
    struct agent_tenant_quota_table *table, const char *tenant)
{
    size_t i;
    for (i = 0U; i < AGENT_TENANT_QUOTA_MAX_SLOTS; i++) {
        if (strcmp(table->slots[i].tenant, tenant) == 0) return &table->slots[i];
    }
    return NULL;
}

static struct agent_tenant_quota_slot *allocate_slot(
    struct agent_tenant_quota_table *table, const char *tenant)
{
    struct agent_tenant_quota_slot *reusable = NULL;
    size_t i;
    int written;
    for (i = 0U; i < AGENT_TENANT_QUOTA_MAX_SLOTS; i++) {
        if (table->slots[i].tenant[0] == '\0') {
            reusable = &table->slots[i];
            break;
        }
        if (reusable == NULL && table->slots[i].active == 0U)
            reusable = &table->slots[i];
    }
    if (reusable == NULL) return NULL;
    memset(reusable, 0, sizeof(*reusable));
    written = snprintf(reusable->tenant, sizeof(reusable->tenant), "%s", tenant);
    if (written < 0 || (size_t)written >= sizeof(reusable->tenant)) {
        memset(reusable, 0, sizeof(*reusable));
        return NULL;
    }
    return reusable;
}

enum agent_tenant_admission_result agent_tenant_quota_acquire(
    struct agent_tenant_quota_table *table, const char *tenant,
    uint32_t max_inflight, uint32_t rate_per_second,
    uint32_t rate_burst, uint64_t now_ms)
{
    struct agent_tenant_quota_slot *slot;
    bool reconfigure_rate;
    if (table == NULL || tenant == NULL || tenant[0] == '\0' ||
        strlen(tenant) >= AGENT_TENANT_QUOTA_ID_LEN ||
        ((rate_per_second == 0U) != (rate_burst == 0U)))
        return AGENT_TENANT_ADMISSION_INVALID;
    if (max_inflight == 0U && rate_per_second == 0U)
        return AGENT_TENANT_ADMITTED;
    slot = find_slot(table, tenant);
    if (slot == NULL) slot = allocate_slot(table, tenant);
    if (slot == NULL) return AGENT_TENANT_TABLE_FULL;
    if (max_inflight != 0U && slot->active >= max_inflight)
        return AGENT_TENANT_CONCURRENCY_LIMITED;
    reconfigure_rate = slot->rate_per_second != rate_per_second ||
                       slot->rate_burst != rate_burst;
    slot->max_inflight = max_inflight;
    slot->rate_per_second = rate_per_second;
    slot->rate_burst = rate_burst;
    slot->rate_enabled = rate_per_second != 0U;
    if (slot->rate_enabled &&
        ((reconfigure_rate && !agent_rate_limiter_init(
             &slot->limiter, rate_per_second, rate_burst, now_ms)) ||
         !agent_rate_limiter_allow(&slot->limiter, now_ms)))
        return AGENT_TENANT_RATE_LIMITED;
    slot->active++;
    return AGENT_TENANT_ADMITTED;
}

bool agent_tenant_quota_release(struct agent_tenant_quota_table *table,
                                const char *tenant)
{
    struct agent_tenant_quota_slot *slot;
    if (table == NULL || tenant == NULL) return false;
    slot = find_slot(table, tenant);
    if (slot == NULL || slot->active == 0U) return false;
    slot->active--;
    return true;
}

#ifndef NEXUS_AGENT_TENANT_QUOTA_H
#define NEXUS_AGENT_TENANT_QUOTA_H

#include "agent_rate_limiter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_TENANT_QUOTA_MAX_SLOTS 64U
#define AGENT_TENANT_QUOTA_ID_LEN 64U

enum agent_tenant_admission_result {
    AGENT_TENANT_ADMITTED = 0,
    AGENT_TENANT_CONCURRENCY_LIMITED,
    AGENT_TENANT_RATE_LIMITED,
    AGENT_TENANT_TABLE_FULL,
    AGENT_TENANT_ADMISSION_INVALID
};

struct agent_tenant_quota_slot {
    char tenant[AGENT_TENANT_QUOTA_ID_LEN];
    uint32_t active;
    uint32_t max_inflight;
    uint32_t rate_per_second;
    uint32_t rate_burst;
    struct agent_rate_limiter limiter;
    bool rate_enabled;
};

struct agent_tenant_quota_table {
    struct agent_tenant_quota_slot slots[AGENT_TENANT_QUOTA_MAX_SLOTS];
};

void agent_tenant_quota_init(struct agent_tenant_quota_table *table);
enum agent_tenant_admission_result agent_tenant_quota_acquire(
    struct agent_tenant_quota_table *table,
    const char *tenant,
    uint32_t max_inflight,
    uint32_t rate_per_second,
    uint32_t rate_burst,
    uint64_t now_ms);
bool agent_tenant_quota_release(struct agent_tenant_quota_table *table,
                                const char *tenant);

#endif

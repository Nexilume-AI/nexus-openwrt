#include "agent_tenant_quota.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct agent_tenant_quota_table table;
    agent_tenant_quota_init(&table);
    assert(agent_tenant_quota_acquire(&table, "tenant-a", 2U, 2U, 2U, 0U) ==
           AGENT_TENANT_ADMITTED);
    assert(agent_tenant_quota_acquire(&table, "tenant-a", 2U, 2U, 2U, 0U) ==
           AGENT_TENANT_ADMITTED);
    assert(agent_tenant_quota_acquire(&table, "tenant-a", 2U, 2U, 2U, 0U) ==
           AGENT_TENANT_CONCURRENCY_LIMITED);
    assert(agent_tenant_quota_release(&table, "tenant-a"));
    assert(agent_tenant_quota_acquire(&table, "tenant-a", 2U, 2U, 2U, 0U) ==
           AGENT_TENANT_RATE_LIMITED);
    assert(agent_tenant_quota_acquire(&table, "tenant-b", 0U, 0U, 0U, 0U) ==
           AGENT_TENANT_ADMITTED);
    assert(!agent_tenant_quota_release(&table, "tenant-b"));
    puts("tenant quota tests passed");
    return 0;
}

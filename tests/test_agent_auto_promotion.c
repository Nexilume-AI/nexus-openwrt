#include "agent_auto_promotion.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct agent_discovery_candidate candidate(
    const char *router_id,
    const char *domain_id
)
{
    struct agent_discovery_candidate value;

    memset(&value, 0, sizeof(value));
    snprintf(value.observation.router_id,
             sizeof(value.observation.router_id), "%s", router_id);
    snprintf(value.observation.domain_id,
             sizeof(value.observation.domain_id), "%s", domain_id);
    return value;
}

int main(void)
{
    struct agent_auto_promotion_policy policy;
    struct agent_discovery_candidate local =
        candidate("router-b", "eda.example");
    struct agent_discovery_candidate remote =
        candidate("router-c", "remote.example");

    assert(agent_auto_promotion_policy_init(
        &policy, "off", "eda.example", "", 30U));
    assert(!agent_auto_promotion_eligible(&policy, &local));
    assert(agent_auto_promotion_policy_init(
        &policy, "same-domain", "eda.example", "", 30U));
    assert(agent_auto_promotion_eligible(&policy, &local));
    assert(!agent_auto_promotion_eligible(&policy, &remote));
    assert(agent_auto_promotion_policy_init(
        &policy, "allowlist", "eda.example", "router-c,router-d", 30U));
    assert(!agent_auto_promotion_eligible(&policy, &local));
    assert(agent_auto_promotion_eligible(&policy, &remote));
    assert(agent_auto_promotion_policy_init(
        &policy, "all", "eda.example", "", 30U));
    assert(agent_auto_promotion_eligible(&policy, &local));
    assert(agent_auto_promotion_eligible(&policy, &remote));
    assert(!agent_auto_promotion_policy_init(
        &policy, "unknown", "eda.example", "", 30U));
    assert(!agent_auto_promotion_policy_init(
        &policy, "allowlist", "eda.example", "router-b,,router-c", 30U));
    assert(!agent_auto_promotion_policy_init(
        &policy, "allowlist", "eda.example", "router-b,", 30U));
    puts("agent auto-promotion tests passed");
    return 0;
}

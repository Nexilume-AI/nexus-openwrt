#include "agent_recovery.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    struct agent_recovery_state state;
    const struct agent_recovery_domain_state *domain;

    agent_recovery_init(&state, 1000U);
    assert(state.generation == 1U);
    assert(agent_recovery_degraded(&state));
    assert(agent_recovery_uptime_ms(&state, 900U) == 0U);
    assert(agent_recovery_uptime_ms(&state, 1600U) == 600U);

    agent_recovery_record_startup(
        &state, AGENT_RECOVERY_STATIC_ROUTES, true, NULL);
    agent_recovery_record_startup(
        &state, AGENT_RECOVERY_POLICY_RIB, false, "invalid policy");
    agent_recovery_record_startup(
        &state, AGENT_RECOVERY_STATIC_PEERS, true, NULL);
    assert(agent_recovery_degraded(&state));
    assert(agent_recovery_failures(&state) == 1U);
    domain = agent_recovery_domain_status(
        &state, AGENT_RECOVERY_POLICY_RIB);
    assert(domain != NULL);
    assert(!domain->configuration_ok);
    assert(domain->startup_attempts == 1U);
    assert(domain->reload_attempts == 0U);
    assert(strcmp(domain->last_error, "invalid policy") == 0);

    agent_recovery_record_reload(
        &state, AGENT_RECOVERY_POLICY_RIB, true, NULL);
    assert(!agent_recovery_degraded(&state));
    assert(domain->configuration_ok);
    assert(domain->reload_attempts == 1U);
    assert(domain->successes == 1U);
    assert(domain->last_error[0] == '\0');
    assert(strcmp(agent_recovery_domain_name(
                      AGENT_RECOVERY_STATIC_ROUTES), "static_routes") == 0);
    assert(strcmp(agent_recovery_domain_name(
                      AGENT_RECOVERY_DOMAIN_COUNT), "unknown") == 0);

    agent_recovery_record_reload(
        &state, AGENT_RECOVERY_STATIC_PEERS, false, NULL);
    assert(agent_recovery_degraded(&state));
    domain = agent_recovery_domain_status(
        &state, AGENT_RECOVERY_STATIC_PEERS);
    assert(strcmp(domain->last_error,
                  "configuration validation failed") == 0);
    assert(agent_recovery_failures(&state) == 2U);
    return 0;
}

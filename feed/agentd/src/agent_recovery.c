#include "agent_recovery.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) (*value)++;
}

static bool valid_domain(enum agent_recovery_domain domain)
{
    return domain >= AGENT_RECOVERY_STATIC_ROUTES &&
           domain < AGENT_RECOVERY_DOMAIN_COUNT;
}

static void record(struct agent_recovery_state *state,
                   enum agent_recovery_domain domain,
                   bool startup, bool success, const char *error)
{
    struct agent_recovery_domain_state *status;

    if (state == NULL || !valid_domain(domain)) return;
    status = &state->domains[domain];
    if (startup) increment(&status->startup_attempts);
    else increment(&status->reload_attempts);
    if (success) {
        increment(&status->successes);
        status->last_error[0] = '\0';
    } else {
        increment(&status->failures);
        (void)snprintf(status->last_error, sizeof(status->last_error), "%s",
                       error != NULL && error[0] != '\0' ? error :
                       "configuration validation failed");
    }
    status->configuration_ok = success;
    increment(&state->generation);
}

void agent_recovery_init(struct agent_recovery_state *state,
                         uint64_t process_started_ms)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->process_started_ms = process_started_ms;
    state->generation = 1U;
}

void agent_recovery_record_startup(struct agent_recovery_state *state,
                                   enum agent_recovery_domain domain,
                                   bool success, const char *error)
{
    record(state, domain, true, success, error);
}

void agent_recovery_record_reload(struct agent_recovery_state *state,
                                  enum agent_recovery_domain domain,
                                  bool success, const char *error)
{
    record(state, domain, false, success, error);
}

bool agent_recovery_degraded(const struct agent_recovery_state *state)
{
    size_t index;

    if (state == NULL) return true;
    for (index = 0U; index < AGENT_RECOVERY_DOMAIN_COUNT; index++) {
        if (!state->domains[index].configuration_ok) return true;
    }
    return false;
}

uint64_t agent_recovery_uptime_ms(const struct agent_recovery_state *state,
                                  uint64_t now_ms)
{
    if (state == NULL || now_ms < state->process_started_ms) return 0U;
    return now_ms - state->process_started_ms;
}

uint64_t agent_recovery_failures(const struct agent_recovery_state *state)
{
    uint64_t total = 0U;
    size_t index;

    if (state == NULL) return 0U;
    for (index = 0U; index < AGENT_RECOVERY_DOMAIN_COUNT; index++) {
        if (UINT64_MAX - total < state->domains[index].failures) {
            return UINT64_MAX;
        }
        total += state->domains[index].failures;
    }
    return total;
}

const char *agent_recovery_domain_name(enum agent_recovery_domain domain)
{
    switch (domain) {
    case AGENT_RECOVERY_STATIC_ROUTES: return "static_routes";
    case AGENT_RECOVERY_POLICY_RIB: return "policy_rib";
    case AGENT_RECOVERY_STATIC_PEERS: return "static_peers";
    default: return "unknown";
    }
}

const struct agent_recovery_domain_state *agent_recovery_domain_status(
    const struct agent_recovery_state *state,
    enum agent_recovery_domain domain)
{
    if (state == NULL || !valid_domain(domain)) return NULL;
    return &state->domains[domain];
}

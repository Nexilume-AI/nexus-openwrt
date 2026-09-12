#ifndef NEXUS_AGENT_RECOVERY_H
#define NEXUS_AGENT_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_RECOVERY_ERROR_LEN 256U

enum agent_recovery_domain {
    AGENT_RECOVERY_STATIC_ROUTES = 0,
    AGENT_RECOVERY_POLICY_RIB,
    AGENT_RECOVERY_STATIC_PEERS,
    AGENT_RECOVERY_DOMAIN_COUNT
};

struct agent_recovery_domain_state {
    bool configuration_ok;
    uint64_t startup_attempts;
    uint64_t reload_attempts;
    uint64_t successes;
    uint64_t failures;
    char last_error[AGENT_RECOVERY_ERROR_LEN];
};

struct agent_recovery_state {
    uint64_t process_started_ms;
    uint64_t generation;
    struct agent_recovery_domain_state domains[AGENT_RECOVERY_DOMAIN_COUNT];
};

void agent_recovery_init(struct agent_recovery_state *state,
                         uint64_t process_started_ms);
void agent_recovery_record_startup(struct agent_recovery_state *state,
                                   enum agent_recovery_domain domain,
                                   bool success, const char *error);
void agent_recovery_record_reload(struct agent_recovery_state *state,
                                  enum agent_recovery_domain domain,
                                  bool success, const char *error);
bool agent_recovery_degraded(const struct agent_recovery_state *state);
uint64_t agent_recovery_uptime_ms(const struct agent_recovery_state *state,
                                  uint64_t now_ms);
uint64_t agent_recovery_failures(const struct agent_recovery_state *state);
const char *agent_recovery_domain_name(enum agent_recovery_domain domain);
const struct agent_recovery_domain_state *agent_recovery_domain_status(
    const struct agent_recovery_state *state,
    enum agent_recovery_domain domain);

#endif

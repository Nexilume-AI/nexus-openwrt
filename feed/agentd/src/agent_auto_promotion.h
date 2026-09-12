#ifndef NEXUS_AGENT_AUTO_PROMOTION_H
#define NEXUS_AGENT_AUTO_PROMOTION_H

#include "agent_discovery.h"
#include "peer_table.h"

#include <stdbool.h>
#include <stdint.h>

#define AGENT_AUTO_PROMOTION_ALLOWLIST_LEN 1025

enum agent_auto_promotion_mode {
    AGENT_AUTO_PROMOTION_OFF = 0,
    AGENT_AUTO_PROMOTION_SAME_DOMAIN = 1,
    AGENT_AUTO_PROMOTION_ALLOWLIST = 2,
    AGENT_AUTO_PROMOTION_ALL = 3
};

struct agent_auto_promotion_policy {
    enum agent_auto_promotion_mode mode;
    char local_domain[AGENT_DOMAIN_ID_LEN];
    char allowlist[AGENT_AUTO_PROMOTION_ALLOWLIST_LEN];
    uint32_t graceful_restart_seconds;
};

bool agent_auto_promotion_policy_init(
    struct agent_auto_promotion_policy *policy,
    const char *mode,
    const char *local_domain,
    const char *allowlist,
    uint32_t graceful_restart_seconds
);

bool agent_auto_promotion_eligible(
    const struct agent_auto_promotion_policy *policy,
    const struct agent_discovery_candidate *candidate
);

const char *agent_auto_promotion_mode_name(
    enum agent_auto_promotion_mode mode
);

#endif

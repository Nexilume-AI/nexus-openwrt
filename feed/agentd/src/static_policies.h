#ifndef NEXUS_AGENT_STATIC_POLICIES_H
#define NEXUS_AGENT_STATIC_POLICIES_H

#include "agent_policy.h"

#include <stddef.h>

#define STATIC_POLICY_ERROR_LEN 256U

struct static_policy_load_result {
    size_t loaded;
    char error[STATIC_POLICY_ERROR_LEN];
};

bool static_policies_reload(struct agent_policy_table *live_table,
                            const char *uci_package_name,
                            struct static_policy_load_result *result);

#endif

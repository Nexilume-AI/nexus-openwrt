#ifndef NEXUS_AGENT_LOCAL_AGENTS_H
#define NEXUS_AGENT_LOCAL_AGENTS_H

#include "route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct agent_local_summary {
    char agent_id[AGENT_URI_LEN];
    char endpoint[AGENT_URI_LEN];
    char tenant[AGENT_TENANT_LEN];
    size_t capability_count;
    size_t healthy_capability_count;
    uint64_t earliest_expires_ms;
    uint64_t latest_expires_ms;
};

struct agent_local_collection {
    size_t count;
    bool truncated;
};

/*
 * The current attachment contract is lease based: a local Agent registers one
 * or more capability routes.  A provider identity is the stable tuple
 * (origin, endpoint, tenant).  This deliberately does not claim that agentd
 * owns or observes an application transport socket.
 */
struct agent_local_collection agent_local_agents_collect(
    const struct route_table *table,
    uint64_t now_ms,
    struct agent_local_summary *summaries,
    size_t capacity
);

bool agent_local_summary_matches(
    const struct agent_local_summary *summary,
    const struct agent_route *route
);

#endif

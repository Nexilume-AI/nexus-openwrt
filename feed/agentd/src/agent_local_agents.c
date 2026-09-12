#include "agent_local_agents.h"

#include <string.h>

bool agent_local_summary_matches(
    const struct agent_local_summary *summary,
    const struct agent_route *route
)
{
    return summary != NULL && route != NULL &&
           route->source == AGENT_ROUTE_SOURCE_LOCAL &&
           strcmp(summary->agent_id, route->origin) == 0 &&
           strcmp(summary->endpoint, route->endpoint) == 0 &&
           strcmp(summary->tenant, route->tenant) == 0;
}

struct agent_local_collection agent_local_agents_collect(
    const struct route_table *table,
    uint64_t now_ms,
    struct agent_local_summary *summaries,
    size_t capacity
)
{
    struct agent_local_collection result = {0U, false};
    const struct agent_route *route;
    size_t index;

    if (table == NULL || summaries == NULL || capacity == 0U) {
        return result;
    }
    memset(summaries, 0, capacity * sizeof(*summaries));

    for (route = route_table_first(table); route != NULL; route = route->next) {
        if (route->source != AGENT_ROUTE_SOURCE_LOCAL ||
            route->lease_expires_ms <= now_ms) {
            continue;
        }
        for (index = 0U; index < result.count; index++) {
            if (agent_local_summary_matches(&summaries[index], route)) {
                break;
            }
        }
        if (index == result.count) {
            if (result.count == capacity) {
                result.truncated = true;
                continue;
            }
            (void)strncpy(summaries[index].agent_id, route->origin,
                          sizeof(summaries[index].agent_id) - 1U);
            (void)strncpy(summaries[index].endpoint, route->endpoint,
                          sizeof(summaries[index].endpoint) - 1U);
            (void)strncpy(summaries[index].tenant, route->tenant,
                          sizeof(summaries[index].tenant) - 1U);
            summaries[index].earliest_expires_ms = route->lease_expires_ms;
            summaries[index].latest_expires_ms = route->lease_expires_ms;
            result.count++;
        }
        summaries[index].capability_count++;
        if (route->healthy) {
            summaries[index].healthy_capability_count++;
        }
        if (route->lease_expires_ms < summaries[index].earliest_expires_ms) {
            summaries[index].earliest_expires_ms = route->lease_expires_ms;
        }
        if (route->lease_expires_ms > summaries[index].latest_expires_ms) {
            summaries[index].latest_expires_ms = route->lease_expires_ms;
        }
    }
    return result;
}

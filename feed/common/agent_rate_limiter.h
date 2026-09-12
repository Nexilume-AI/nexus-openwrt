#ifndef NEXUS_AGENT_RATE_LIMITER_H
#define NEXUS_AGENT_RATE_LIMITER_H

#include <stdbool.h>
#include <stdint.h>

struct agent_rate_limiter {
    uint32_t rate_per_second;
    uint32_t burst;
    uint64_t tokens_milli;
    uint64_t last_refill_ms;
};

bool agent_rate_limiter_init(
    struct agent_rate_limiter *limiter,
    uint32_t rate_per_second,
    uint32_t burst,
    uint64_t now_ms
);

bool agent_rate_limiter_allow(
    struct agent_rate_limiter *limiter,
    uint64_t now_ms
);

#endif

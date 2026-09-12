#include "agent_rate_limiter.h"

#include <stddef.h>

#define AGENT_RATE_TOKEN_SCALE 1000U

bool agent_rate_limiter_init(
    struct agent_rate_limiter *limiter,
    uint32_t rate_per_second,
    uint32_t burst,
    uint64_t now_ms
)
{
    if (limiter == NULL || rate_per_second == 0U || burst == 0U) {
        return false;
    }
    limiter->rate_per_second = rate_per_second;
    limiter->burst = burst;
    limiter->tokens_milli = (uint64_t)burst * AGENT_RATE_TOKEN_SCALE;
    limiter->last_refill_ms = now_ms;
    return true;
}

bool agent_rate_limiter_allow(
    struct agent_rate_limiter *limiter,
    uint64_t now_ms
)
{
    uint64_t capacity;
    uint64_t rate;
    uint64_t elapsed;
    uint64_t full_refill_ms;
    uint64_t refill;

    if (limiter == NULL ||
        limiter->rate_per_second == 0U ||
        limiter->burst == 0U) {
        return false;
    }

    capacity = (uint64_t)limiter->burst * AGENT_RATE_TOKEN_SCALE;
    rate = (uint64_t)limiter->rate_per_second;
    if (now_ms > limiter->last_refill_ms) {
        elapsed = now_ms - limiter->last_refill_ms;
        full_refill_ms = (capacity + rate - 1U) / rate;
        if (elapsed >= full_refill_ms) {
            limiter->tokens_milli = capacity;
        } else {
            refill = elapsed * rate;
            if (refill > capacity - limiter->tokens_milli) {
                limiter->tokens_milli = capacity;
            } else {
                limiter->tokens_milli += refill;
            }
        }
        limiter->last_refill_ms = now_ms;
    }

    if (limiter->tokens_milli < AGENT_RATE_TOKEN_SCALE) {
        return false;
    }
    limiter->tokens_milli -= AGENT_RATE_TOKEN_SCALE;
    return true;
}

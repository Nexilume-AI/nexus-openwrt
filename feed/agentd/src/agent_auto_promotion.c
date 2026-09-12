#include "agent_auto_promotion.h"

#include <stdio.h>
#include <string.h>

static bool copy_optional(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || capacity == 0U || source == NULL) return false;
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static bool parse_mode(
    const char *text,
    enum agent_auto_promotion_mode *mode
)
{
    if (text == NULL || mode == NULL) return false;
    if (strcmp(text, "off") == 0) *mode = AGENT_AUTO_PROMOTION_OFF;
    else if (strcmp(text, "same-domain") == 0) {
        *mode = AGENT_AUTO_PROMOTION_SAME_DOMAIN;
    } else if (strcmp(text, "allowlist") == 0) {
        *mode = AGENT_AUTO_PROMOTION_ALLOWLIST;
    } else if (strcmp(text, "all") == 0) {
        *mode = AGENT_AUTO_PROMOTION_ALL;
    } else return false;
    return true;
}

static bool allowlist_valid(const char *text)
{
    const char *start;
    const char *comma;
    char identifier[AGENT_PEER_ID_LEN];
    size_t length;

    if (text == NULL || text[0] == '\0') return true;
    if (text[strlen(text) - 1U] == ',') return false;
    for (start = text; *start != '\0'; start = comma + 1U) {
        comma = strchr(start, ',');
        length = comma == NULL ? strlen(start) : (size_t)(comma - start);
        if (length == 0U || length >= sizeof(identifier)) return false;
        memcpy(identifier, start, length);
        identifier[length] = '\0';
        if (!agent_peer_id_valid(identifier, sizeof(identifier))) return false;
        if (comma == NULL) break;
    }
    return true;
}

static bool allowlist_contains(const char *text, const char *router_id)
{
    const char *start;
    const char *comma;
    size_t length;
    size_t router_length;

    if (text == NULL || router_id == NULL) return false;
    router_length = strlen(router_id);
    for (start = text; *start != '\0'; start = comma + 1U) {
        comma = strchr(start, ',');
        length = comma == NULL ? strlen(start) : (size_t)(comma - start);
        if (length == router_length &&
            strncmp(start, router_id, length) == 0) return true;
        if (comma == NULL) break;
    }
    return false;
}

bool agent_auto_promotion_policy_init(
    struct agent_auto_promotion_policy *policy,
    const char *mode,
    const char *local_domain,
    const char *allowlist,
    uint32_t graceful_restart_seconds
)
{
    if (policy == NULL || local_domain == NULL ||
        !agent_peer_domain_valid(local_domain) ||
        graceful_restart_seconds == 0U ||
        !allowlist_valid(allowlist)) return false;
    memset(policy, 0, sizeof(*policy));
    if (!parse_mode(mode, &policy->mode) ||
        !copy_optional(policy->local_domain,
                       sizeof(policy->local_domain), local_domain) ||
        !copy_optional(policy->allowlist,
                       sizeof(policy->allowlist), allowlist)) return false;
    policy->graceful_restart_seconds = graceful_restart_seconds;
    return true;
}

bool agent_auto_promotion_eligible(
    const struct agent_auto_promotion_policy *policy,
    const struct agent_discovery_candidate *candidate
)
{
    const struct agent_discovery_observation *observation;

    if (policy == NULL || candidate == NULL) return false;
    observation = &candidate->observation;
    switch (policy->mode) {
    case AGENT_AUTO_PROMOTION_SAME_DOMAIN:
        return strcmp(observation->domain_id, policy->local_domain) == 0;
    case AGENT_AUTO_PROMOTION_ALLOWLIST:
        return allowlist_contains(policy->allowlist,
                                  observation->router_id);
    case AGENT_AUTO_PROMOTION_ALL:
        return true;
    case AGENT_AUTO_PROMOTION_OFF:
    default:
        return false;
    }
}

const char *agent_auto_promotion_mode_name(
    enum agent_auto_promotion_mode mode
)
{
    switch (mode) {
    case AGENT_AUTO_PROMOTION_OFF: return "off";
    case AGENT_AUTO_PROMOTION_SAME_DOMAIN: return "same-domain";
    case AGENT_AUTO_PROMOTION_ALLOWLIST: return "allowlist";
    case AGENT_AUTO_PROMOTION_ALL: return "all";
    default: return "unknown";
    }
}

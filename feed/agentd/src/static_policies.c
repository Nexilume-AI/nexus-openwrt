#include "static_policies.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uci.h>

static void set_error(struct static_policy_load_result *result,
                      const char *section, const char *message)
{
    if (result == NULL) return;
    if (section == NULL) snprintf(result->error, sizeof(result->error), "%s", message);
    else snprintf(result->error, sizeof(result->error),
                  "policy section '%s': %s", section, message);
}

static const char *option(struct uci_context *context,
                          struct uci_section *section, const char *name)
{
    return uci_lookup_option_string(context, section, name);
}

static bool copy_optional(char *target, size_t capacity, const char *source,
                          const char *fallback)
{
    const char *value = source == NULL ? fallback : source;
    int written;
    if (target == NULL || value == NULL) return false;
    written = snprintf(target, capacity, "%s", value);
    return written >= 0 && (size_t)written < capacity;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || value == NULL || text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (!parse_u64(text, &parsed) || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool optional_u64(struct uci_context *context, struct uci_section *section,
                         const char *name, uint64_t fallback, uint64_t *value)
{
    const char *text = option(context, section, name);
    if (text == NULL) { *value = fallback; return true; }
    return parse_u64(text, value);
}

static bool optional_u32(struct uci_context *context, struct uci_section *section,
                         const char *name, uint32_t fallback, uint32_t *value)
{
    const char *text = option(context, section, name);
    if (text == NULL) { *value = fallback; return true; }
    return parse_u32(text, value);
}

static bool parse_bool(const char *text, bool fallback, bool *value)
{
    if (text == NULL) { *value = fallback; return true; }
    if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
        strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0) {
        *value = true; return true;
    }
    if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
        strcasecmp(text, "no") == 0 || strcasecmp(text, "off") == 0) {
        *value = false; return true;
    }
    return false;
}

static bool parse_source_mask(const char *text, uint8_t *mask)
{
    char copy[128];
    char *token;
    char *save = NULL;
    uint8_t parsed = 0U;
    if (text == NULL || strcmp(text, "*") == 0 || strcmp(text, "all") == 0) {
        *mask = AGENT_POLICY_SOURCE_MASK_ALL; return true;
    }
    if (snprintf(copy, sizeof(copy), "%s", text) < 0 || strlen(text) >= sizeof(copy)) return false;
    token = strtok_r(copy, ", ", &save);
    while (token != NULL) {
        if (strcmp(token, "local") == 0) parsed |= 1U << AGENT_ROUTE_SOURCE_LOCAL;
        else if (strcmp(token, "static") == 0) parsed |= 1U << AGENT_ROUTE_SOURCE_STATIC;
        else if (strcmp(token, "peer") == 0) parsed |= 1U << AGENT_ROUTE_SOURCE_PEER;
        else if (strcmp(token, "discovery") == 0) parsed |= 1U << AGENT_ROUTE_SOURCE_DISCOVERY;
        else return false;
        token = strtok_r(NULL, ", ", &save);
    }
    if (parsed == 0U) return false;
    *mask = parsed;
    return true;
}

static bool parse_core(struct uci_context *context, struct uci_section *section,
                       struct agent_policy_table *candidate,
                       struct static_policy_load_result *result)
{
    const char *action = option(context, section, "default_action");
    uint32_t value;
    if (action != NULL && strcmp(action, "allow") != 0 && strcmp(action, "deny") != 0) {
        set_error(result, section->e.name, "default_action must be allow or deny"); return false;
    }
    candidate->default_allow = action == NULL || strcmp(action, "allow") == 0;
    if (!optional_u32(context, section, "health_failure_threshold", 1U, &value) ||
        value == 0U || value > UINT8_MAX) {
        set_error(result, section->e.name, "health_failure_threshold must be 1..255"); return false;
    }
    candidate->health_failure_threshold = (uint8_t)value;
    if (!optional_u32(context, section, "health_recovery_threshold", 1U, &value) ||
        value == 0U || value > UINT8_MAX) {
        set_error(result, section->e.name, "health_recovery_threshold must be 1..255"); return false;
    }
    candidate->health_recovery_threshold = (uint8_t)value;
    return true;
}

static bool parse_rule(struct uci_context *context, struct uci_section *section,
                       struct agent_policy_rule *rule,
                       struct static_policy_load_result *result)
{
    const char *action;
    uint32_t value;
    memset(rule, 0, sizeof(*rule));
    if (!copy_optional(rule->policy_id, sizeof(rule->policy_id),
                       option(context, section, "policy_id"), NULL)) {
        set_error(result, section->e.name, "policy_id is required and must fit 64 bytes"); return false;
    }
    action = option(context, section, "action");
    if (action == NULL || (strcmp(action, "allow") != 0 && strcmp(action, "deny") != 0)) {
        set_error(result, section->e.name, "action must be allow or deny"); return false;
    }
    rule->action = strcmp(action, "deny") == 0 ? AGENT_POLICY_DENY : AGENT_POLICY_ALLOW;
    if (!optional_u32(context, section, "priority", 100U, &rule->priority) ||
        !copy_optional(rule->match_tenant, sizeof(rule->match_tenant), option(context, section, "tenant"), "*") ||
        !copy_optional(rule->match_source_agent, sizeof(rule->match_source_agent), option(context, section, "source_agent"), "*") ||
        !copy_optional(rule->match_intent, sizeof(rule->match_intent), option(context, section, "intent"), "*") ||
        !copy_optional(rule->required_region, sizeof(rule->required_region), option(context, section, "required_region"), "") ||
        !copy_optional(rule->required_peer, sizeof(rule->required_peer), option(context, section, "required_peer"), "") ||
        !copy_optional(rule->denied_peer, sizeof(rule->denied_peer), option(context, section, "denied_peer"), "") ||
        !copy_optional(rule->preferred_peer, sizeof(rule->preferred_peer), option(context, section, "preferred_peer"), "") ||
        !copy_optional(rule->allowed_endpoint_prefix,
                       sizeof(rule->allowed_endpoint_prefix),
                       option(context, section, "allowed_endpoint_prefix"), "") ||
        !optional_u64(context, section, "max_cost_microunits", 0U, &rule->max_cost_microunits) ||
        !optional_u32(context, section, "max_latency_ms", 0U, &rule->max_latency_ms) ||
        !optional_u32(context, section, "min_trust", 0U, &value) || value > 100U) {
        set_error(result, section->e.name, "invalid match or route constraint"); return false;
    }
    rule->min_trust_level = (uint8_t)value;
    if (!optional_u32(context, section, "max_load_permille", 0U, &value) || value > 1000U) {
        set_error(result, section->e.name, "max_load_permille must be 0..1000"); return false;
    }
    rule->max_load_permille = (uint16_t)value;
    if (!optional_u32(context, section, "max_hops", 0U, &value) || value > UINT8_MAX) {
        set_error(result, section->e.name, "max_hops must be 0..255"); return false;
    }
    rule->max_hops = (uint8_t)value;
    if (!parse_source_mask(option(context, section, "route_sources"), &rule->source_mask) ||
        !optional_u64(context, section, "nonpreferred_peer_penalty", 250000U, &rule->nonpreferred_peer_penalty) ||
        !optional_u32(context, section, "latency_weight", 1000U, &rule->latency_weight) ||
        !optional_u32(context, section, "cost_divisor", 100U, &rule->cost_divisor) || rule->cost_divisor == 0U ||
        !optional_u32(context, section, "load_weight", 100U, &rule->load_weight) ||
        !optional_u32(context, section, "trust_weight", 5000U, &rule->trust_weight) ||
        !optional_u32(context, section, "hop_weight", 10000U, &rule->hop_weight)) {
        set_error(result, section->e.name, "invalid source mask or TE weight"); return false;
    }
    if (!optional_u32(context, section, "tenant_max_inflight", 0U,
                      &rule->tenant_max_inflight) ||
        !optional_u32(context, section, "tenant_rate_per_second", 0U,
                      &rule->tenant_rate_per_second) ||
        !optional_u32(context, section, "tenant_rate_burst", 0U,
                      &rule->tenant_rate_burst) ||
        ((rule->tenant_rate_per_second == 0U) !=
         (rule->tenant_rate_burst == 0U))) {
        set_error(result, section->e.name,
                  "tenant rate and burst must both be zero or both positive");
        return false;
    }
    return true;
}

bool static_policies_reload(struct agent_policy_table *live_table,
                            const char *uci_package_name,
                            struct static_policy_load_result *result)
{
    struct agent_policy_table candidate;
    struct uci_context *context;
    struct uci_package *package = NULL;
    struct uci_element *element;
    struct uci_section *section;
    struct agent_policy_rule rule;
    bool enabled;
    bool core_seen = false;

    if (live_table == NULL || uci_package_name == NULL || result == NULL) return false;
    memset(result, 0, sizeof(*result));
    agent_policy_table_init(&candidate);
    context = uci_alloc_context();
    if (context == NULL) { set_error(result, NULL, "failed to allocate UCI context"); return false; }
    if (uci_load(context, uci_package_name, &package) != UCI_OK) {
        set_error(result, NULL, "failed to load UCI package"); uci_free_context(context); return false;
    }
    uci_foreach_element(&package->sections, element) {
        section = uci_to_section(element);
        if (strcmp(section->type, "core") == 0) {
            if (core_seen || !parse_core(context, section, &candidate, result)) goto fail;
            core_seen = true; continue;
        }
        if (strcmp(section->type, "policy") != 0) continue;
        if (!parse_bool(option(context, section, "enabled"), true, &enabled)) {
            set_error(result, section->e.name, "enabled must be boolean"); goto fail;
        }
        if (!enabled) continue;
        if (!parse_rule(context, section, &rule, result) ||
            !agent_policy_table_add(&candidate, &rule)) {
            if (result->error[0] == '\0') set_error(result, section->e.name, "duplicate policy_id or policy table full");
            goto fail;
        }
        result->loaded++;
    }
    candidate.generation = live_table->generation == UINT64_MAX
        ? UINT64_MAX : live_table->generation + 1U;
    *live_table = candidate;
    uci_unload(context, package);
    uci_free_context(context);
    return true;
fail:
    result->loaded = 0U;
    uci_unload(context, package);
    uci_free_context(context);
    return false;
}

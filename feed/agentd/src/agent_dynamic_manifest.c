#include "agent_dynamic_manifest.h"

#include <string.h>

static bool bounded(const char *value, size_t capacity)
{
    return value != NULL && value[0] != '\0' &&
           memchr(value, '\0', capacity) != NULL;
}

static bool lowercase_sha256(const char *value)
{
    size_t index;

    if (!bounded(value, AGENT_DYNAMIC_MANIFEST_DIGEST_LEN) ||
        strlen(value) != 64U) return false;
    for (index = 0U; index < 64U; index++) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

static bool same_tool_identity(
    const struct agent_dynamic_manifest_entry *left,
    const struct agent_dynamic_manifest_entry *right
)
{
    return strcmp(left->origin, right->origin) == 0 &&
           strcmp(left->mapping.selector, right->mapping.selector) == 0 &&
           strcmp(left->mapping.intent, right->mapping.intent) == 0;
}

void agent_dynamic_manifest_init(struct agent_dynamic_manifest_table *table)
{
    if (table != NULL) memset(table, 0, sizeof(*table));
}

enum agent_dynamic_manifest_result agent_dynamic_manifest_validate_upsert(
    const struct agent_dynamic_manifest_table *table,
    const struct agent_dynamic_manifest_entry *entry
)
{
    size_t index;
    size_t origin_tools = 0U;
    bool replacing = false;

    if (table == NULL || entry == NULL ||
        !bounded(entry->route_id, sizeof(entry->route_id)) ||
        !bounded(entry->tenant, sizeof(entry->tenant)) ||
        !bounded(entry->origin, sizeof(entry->origin)) ||
        !bounded(entry->agent_name, sizeof(entry->agent_name)) ||
        !lowercase_sha256(entry->manifest_digest) ||
        !entry->mapping.enabled ||
        entry->mapping.protocol != AGENT_ADAPTER_PROTOCOL_MCP ||
        strcmp(entry->mapping.authority, entry->origin) != 0 ||
        (entry->computer_present &&
         (entry->computer_requirement > AGENT_IPC_COMPUTER_REQUIRED ||
          (entry->workspace_capabilities &
           (uint16_t)~AGENT_IPC_WORKSPACE_ALL) != 0U ||
          (entry->computer_requirement == AGENT_IPC_COMPUTER_DISABLED &&
           entry->workspace_capabilities != 0U))) ||
        (!entry->computer_present &&
         (entry->computer_requirement != AGENT_IPC_COMPUTER_DISABLED ||
          entry->workspace_capabilities != 0U)) ||
        (entry->mobile_present &&
         (entry->mobile_requirement > AGENT_IPC_MOBILE_REQUIRED ||
          (entry->mobile_capabilities &
           (uint16_t)~AGENT_IPC_MOBILE_ALL) != 0U ||
          (entry->mobile_scopes &
           (uint16_t)~AGENT_IPC_MOBILE_ALL) != 0U ||
          (entry->mobile_requirement == AGENT_IPC_MOBILE_DISABLED &&
           entry->mobile_capabilities != 0U) ||
          (entry->mobile_requirement == AGENT_IPC_MOBILE_REQUIRED &&
           entry->mobile_capabilities == 0U) ||
          (entry->mobile_scopes &
           (uint16_t)~entry->mobile_capabilities) != 0U)) ||
        (!entry->mobile_present &&
         (entry->mobile_requirement != AGENT_IPC_MOBILE_DISABLED ||
          entry->mobile_capabilities != 0U ||
          entry->mobile_scopes != 0U)) ||
        !agent_adapter_mapping_valid(&entry->mapping)) {
        return AGENT_DYNAMIC_MANIFEST_INVALID;
    }
    for (index = 0U; index < table->count; index++) {
        const struct agent_dynamic_manifest_entry *current =
            &table->entries[index];

        if (strcmp(current->route_id, entry->route_id) == 0) {
            replacing = true;
            continue;
        }
        if (strcmp(current->origin, entry->origin) != 0) continue;
        if (strcmp(current->tenant, entry->tenant) != 0)
            return AGENT_DYNAMIC_MANIFEST_TENANT_CONFLICT;
        if (strcmp(current->agent_name, entry->agent_name) != 0)
            return AGENT_DYNAMIC_MANIFEST_NAME_CONFLICT;
        if (current->computer_present != entry->computer_present ||
            current->computer_requirement != entry->computer_requirement ||
            current->workspace_capabilities != entry->workspace_capabilities)
            return AGENT_DYNAMIC_MANIFEST_COMPUTER_POLICY_CONFLICT;
        if (current->mobile_present != entry->mobile_present ||
            current->mobile_requirement != entry->mobile_requirement ||
            current->mobile_capabilities != entry->mobile_capabilities)
            return AGENT_DYNAMIC_MANIFEST_MOBILE_POLICY_CONFLICT;
        if (same_tool_identity(current, entry)) {
            replacing = true;
            continue;
        }
        origin_tools++;
        if (strcmp(current->mapping.selector, entry->mapping.selector) == 0)
            return AGENT_DYNAMIC_MANIFEST_SELECTOR_CONFLICT;
    }
    if (!replacing && table->count >= AGENT_DYNAMIC_MANIFEST_MAX_TOOLS)
        return AGENT_DYNAMIC_MANIFEST_FULL;
    if (origin_tools >= AGENT_DYNAMIC_MANIFEST_MAX_ORIGIN_TOOLS)
        return AGENT_DYNAMIC_MANIFEST_ORIGIN_FULL;
    return AGENT_DYNAMIC_MANIFEST_OK;
}

enum agent_dynamic_manifest_result agent_dynamic_manifest_upsert(
    struct agent_dynamic_manifest_table *table,
    const struct agent_dynamic_manifest_entry *entry
)
{
    enum agent_dynamic_manifest_result result;
    size_t index;

    result = agent_dynamic_manifest_validate_upsert(table, entry);
    if (result != AGENT_DYNAMIC_MANIFEST_OK) return result;
    for (index = 0U; index < table->count; index++) {
        if (strcmp(table->entries[index].route_id, entry->route_id) == 0 ||
            same_tool_identity(&table->entries[index], entry)) {
            table->entries[index] = *entry;
            table->generation++;
            return AGENT_DYNAMIC_MANIFEST_OK;
        }
    }
    table->entries[table->count++] = *entry;
    table->generation++;
    return AGENT_DYNAMIC_MANIFEST_OK;
}

bool agent_dynamic_manifest_remove(
    struct agent_dynamic_manifest_table *table,
    const char *route_id
)
{
    size_t index;

    if (table == NULL || route_id == NULL) return false;
    for (index = 0U; index < table->count; index++) {
        if (strcmp(table->entries[index].route_id, route_id) != 0) continue;
        if (index + 1U < table->count) {
            memmove(&table->entries[index], &table->entries[index + 1U],
                    (table->count - index - 1U) *
                        sizeof(table->entries[0]));
        }
        memset(&table->entries[table->count - 1U], 0,
               sizeof(table->entries[0]));
        table->count--;
        table->generation++;
        return true;
    }
    return false;
}

size_t agent_dynamic_manifest_prune(
    struct agent_dynamic_manifest_table *table,
    agent_dynamic_manifest_route_active active,
    void *context
)
{
    size_t index = 0U;
    size_t removed = 0U;

    if (table == NULL || active == NULL) return 0U;
    while (index < table->count) {
        if (active(table->entries[index].route_id, context)) {
            index++;
            continue;
        }
        if (index + 1U < table->count) {
            memmove(&table->entries[index], &table->entries[index + 1U],
                    (table->count - index - 1U) *
                        sizeof(table->entries[0]));
        }
        table->count--;
        memset(&table->entries[table->count], 0,
               sizeof(table->entries[0]));
        removed++;
    }
    if (removed > 0U) table->generation++;
    return removed;
}

const char *agent_dynamic_manifest_result_name(
    enum agent_dynamic_manifest_result result
)
{
    switch (result) {
    case AGENT_DYNAMIC_MANIFEST_OK: return "ok";
    case AGENT_DYNAMIC_MANIFEST_INVALID: return "invalid";
    case AGENT_DYNAMIC_MANIFEST_FULL: return "global_tool_limit";
    case AGENT_DYNAMIC_MANIFEST_ORIGIN_FULL: return "origin_tool_limit";
    case AGENT_DYNAMIC_MANIFEST_NAME_CONFLICT: return "agent_name_conflict";
    case AGENT_DYNAMIC_MANIFEST_TENANT_CONFLICT: return "tenant_conflict";
    case AGENT_DYNAMIC_MANIFEST_COMPUTER_POLICY_CONFLICT:
        return "computer_policy_conflict";
    case AGENT_DYNAMIC_MANIFEST_MOBILE_POLICY_CONFLICT:
        return "mobile_policy_conflict";
    case AGENT_DYNAMIC_MANIFEST_SELECTOR_CONFLICT: return "selector_conflict";
    default: return "unknown";
    }
}

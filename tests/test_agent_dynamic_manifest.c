#include "agent_dynamic_manifest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void text(char *target, size_t capacity, const char *value)
{
    int written = snprintf(target, capacity, "%s", value);
    assert(written > 0 && (size_t)written < capacity);
}

static struct agent_dynamic_manifest_entry entry(
    unsigned int number,
    const char *origin,
    const char *name,
    const char *selector
)
{
    struct agent_dynamic_manifest_entry value = {0};

    (void)snprintf(value.route_id, sizeof(value.route_id), "%032x", number);
    text(value.tenant, sizeof(value.tenant), "tenant-local");
    text(value.origin, sizeof(value.origin), origin);
    text(value.agent_name, sizeof(value.agent_name), name);
    text(value.manifest_digest, sizeof(value.manifest_digest),
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    value.publish = true;
    value.computer_present = true;
    value.computer_requirement = AGENT_IPC_COMPUTER_REQUIRED;
    value.workspace_capabilities = AGENT_IPC_WORKSPACE_FILES_READ;
    value.mapping.enabled = true;
    value.mapping.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    text(value.mapping.authority, sizeof(value.mapping.authority), origin);
    text(value.mapping.selector, sizeof(value.mapping.selector), selector);
    (void)snprintf(value.mapping.intent, sizeof(value.mapping.intent),
                   "demo.tool.%u", number);
    value.mapping.intent_version = 1U;
    text(value.mapping.input_schema_json,
         sizeof(value.mapping.input_schema_json), "{}");
    return value;
}

static bool route_active(const char *route_id, void *context)
{
    return strcmp(route_id, (const char *)context) == 0;
}

static void test_upsert_conflicts_and_cleanup(void)
{
    struct agent_dynamic_manifest_table table;
    struct agent_dynamic_manifest_entry first = entry(
        1U, "agent://demo/echo", "Echo", "demo.echo");
    struct agent_dynamic_manifest_entry second = entry(
        2U, "agent://demo/echo", "Echo", "demo.reverse");
    struct agent_dynamic_manifest_entry conflict;

    agent_dynamic_manifest_init(&table);
    assert(agent_dynamic_manifest_upsert(&table, &first) ==
           AGENT_DYNAMIC_MANIFEST_OK);
    assert(strcmp(table.entries[0].tenant, "tenant-local") == 0);
    assert(agent_dynamic_manifest_upsert(&table, &second) ==
           AGENT_DYNAMIC_MANIFEST_OK);
    assert(table.count == 2U);

    conflict = entry(3U, "agent://demo/echo", "Different", "demo.upper");
    assert(agent_dynamic_manifest_upsert(&table, &conflict) ==
           AGENT_DYNAMIC_MANIFEST_NAME_CONFLICT);
    conflict = entry(3U, "agent://demo/echo", "Echo", "demo.echo");
    text(conflict.mapping.intent, sizeof(conflict.mapping.intent),
         first.mapping.intent);
    text(conflict.mapping.description, sizeof(conflict.mapping.description),
         "Updated after Agent restart");
    assert(agent_dynamic_manifest_upsert(&table, &conflict) ==
           AGENT_DYNAMIC_MANIFEST_OK);
    assert(table.count == 2U);
    assert(strcmp(table.entries[0].route_id, conflict.route_id) == 0);
    assert(strcmp(table.entries[0].mapping.description,
                  "Updated after Agent restart") == 0);
    first = conflict;

    conflict = entry(4U, "agent://demo/echo", "Echo", "demo.echo");
    assert(agent_dynamic_manifest_upsert(&table, &conflict) ==
           AGENT_DYNAMIC_MANIFEST_SELECTOR_CONFLICT);
    conflict = entry(4U, "agent://demo/echo", "Echo", "demo.upper");
    text(conflict.tenant, sizeof(conflict.tenant), "tenant-other");
    assert(agent_dynamic_manifest_upsert(&table, &conflict) ==
           AGENT_DYNAMIC_MANIFEST_TENANT_CONFLICT);
    assert(table.count == 2U);

    conflict = entry(4U, "agent://demo/echo", "Echo", "demo.upper");
    conflict.workspace_capabilities |= AGENT_IPC_WORKSPACE_FILES_WRITE;
    assert(agent_dynamic_manifest_upsert(&table, &conflict) ==
           AGENT_DYNAMIC_MANIFEST_COMPUTER_POLICY_CONFLICT);
    assert(strcmp(agent_dynamic_manifest_result_name(
                      AGENT_DYNAMIC_MANIFEST_COMPUTER_POLICY_CONFLICT),
                  "computer_policy_conflict") == 0);

    first.mapping.task = true;
    assert(agent_dynamic_manifest_upsert(&table, &first) ==
           AGENT_DYNAMIC_MANIFEST_OK);
    assert(table.count == 2U);
    assert(table.entries[0].mapping.task);

    assert(agent_dynamic_manifest_prune(
               &table, route_active, first.route_id) == 1U);
    assert(table.count == 1U);
    assert(strcmp(table.entries[0].route_id, first.route_id) == 0);
    assert(agent_dynamic_manifest_remove(&table, first.route_id));
    assert(table.count == 0U);

    first = entry(5U, "agent://demo/invalid", "Invalid", "demo.invalid");
    first.tenant[0] = '\0';
    assert(agent_dynamic_manifest_upsert(&table, &first) ==
           AGENT_DYNAMIC_MANIFEST_INVALID);
}

static void test_per_origin_limit(void)
{
    struct agent_dynamic_manifest_table table;
    struct agent_dynamic_manifest_entry value;
    unsigned int index;

    agent_dynamic_manifest_init(&table);
    for (index = 0U; index < AGENT_DYNAMIC_MANIFEST_MAX_ORIGIN_TOOLS; index++) {
        char selector[AGENT_ADAPTER_SELECTOR_LEN];
        (void)snprintf(selector, sizeof(selector), "tool.%u", index);
        value = entry(index + 1U, "agent://demo/many", "Many", selector);
        assert(agent_dynamic_manifest_upsert(&table, &value) ==
               AGENT_DYNAMIC_MANIFEST_OK);
    }
    value = entry(1000U, "agent://demo/many", "Many", "tool.overflow");
    assert(agent_dynamic_manifest_upsert(&table, &value) ==
           AGENT_DYNAMIC_MANIFEST_ORIGIN_FULL);
}

int main(void)
{
    test_upsert_conflicts_and_cleanup();
    test_per_origin_limit();
    puts("agent dynamic manifest tests passed");
    return 0;
}

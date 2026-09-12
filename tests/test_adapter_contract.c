#include "agent_adapter_contract.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);

    assert(written >= 0);
    assert((size_t)written < capacity);
}

static struct agent_adapter_mapping mapping(
    enum agent_adapter_protocol protocol,
    const char *authority,
    const char *selector,
    const char *intent,
    uint32_t version
)
{
    struct agent_adapter_mapping value = {0};

    value.enabled = true;
    value.protocol = protocol;
    value.intent_version = version;
    set_text(value.authority, sizeof(value.authority), authority);
    set_text(value.selector, sizeof(value.selector), selector);
    set_text(value.intent, sizeof(value.intent), intent);
    return value;
}

static void test_protocol_names(void)
{
    assert(agent_adapter_protocol_parse("mcp") == AGENT_ADAPTER_PROTOCOL_MCP);
    assert(agent_adapter_protocol_parse("a2a") == AGENT_ADAPTER_PROTOCOL_A2A);
    assert(agent_adapter_protocol_parse("MCP") ==
           AGENT_ADAPTER_PROTOCOL_INVALID);
    assert(strcmp(agent_adapter_protocol_name(AGENT_ADAPTER_PROTOCOL_A2A),
                  "a2a") == 0);
}

static void test_exact_registry_lookup(void)
{
    struct agent_adapter_mapping values[] = {
        mapping(AGENT_ADAPTER_PROTOCOL_MCP, "eda-local", "lint_verilog",
                "chip.verilog.verify.lint", 1U),
        mapping(AGENT_ADAPTER_PROTOCOL_A2A, "chip-agent", "lint",
                "chip.verilog.verify.lint", 2U)
    };
    const struct agent_adapter_mapping *match = NULL;

    assert(agent_adapter_registry_validate(values, 2U) == AGENT_ADAPTER_OK);
    assert(agent_adapter_lookup(values, 2U, AGENT_ADAPTER_PROTOCOL_MCP,
                                "eda-local", "lint_verilog", &match) ==
           AGENT_ADAPTER_OK);
    assert(match == &values[0]);
    assert(match->intent_version == 1U);
    assert(agent_adapter_lookup(values, 2U, AGENT_ADAPTER_PROTOCOL_MCP,
                                "eda-local", "lint", &match) ==
           AGENT_ADAPTER_NOT_FOUND);
    assert(match == NULL);
}

static void test_invalid_and_ambiguous_registry(void)
{
    struct agent_adapter_mapping duplicate[] = {
        mapping(AGENT_ADAPTER_PROTOCOL_MCP, "eda-local", "lint",
                "chip.verilog.verify.lint", 1U),
        mapping(AGENT_ADAPTER_PROTOCOL_MCP, "eda-local", "lint",
                "chip.verilog.verify.lint.fast", 1U)
    };
    struct agent_adapter_mapping invalid = mapping(
        AGENT_ADAPTER_PROTOCOL_MCP, "eda local", "lint",
        "chip.verilog.verify.lint", 1U);

    assert(agent_adapter_registry_validate(duplicate, 2U) ==
           AGENT_ADAPTER_AMBIGUOUS);
    assert(agent_adapter_registry_validate(&invalid, 1U) ==
           AGENT_ADAPTER_INVALID);
    invalid = mapping(AGENT_ADAPTER_PROTOCOL_A2A, "chip-agent", "lint",
                      "chip..lint", 1U);
    assert(!agent_adapter_mapping_valid(&invalid));
}

static void test_task_id_mapping(void)
{
    char task_id[AGENT_ADAPTER_TASK_ID_LEN];
    char oversized[AGENT_ADAPTER_TASK_ID_LEN];

    assert(agent_adapter_make_task_id(AGENT_ADAPTER_PROTOCOL_MCP, "42",
                                      task_id));
    assert(strcmp(task_id, "mcp:42") == 0);
    assert(agent_adapter_make_task_id(AGENT_ADAPTER_PROTOCOL_A2A,
                                      "message-018f", task_id));
    assert(strcmp(task_id, "a2a:message-018f") == 0);
    assert(!agent_adapter_make_task_id(AGENT_ADAPTER_PROTOCOL_MCP,
                                       "request with spaces", task_id));
    memset(oversized, 'a', sizeof(oversized));
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(!agent_adapter_make_task_id(AGENT_ADAPTER_PROTOCOL_MCP,
                                       oversized, task_id));
}

int main(void)
{
    test_protocol_names();
    test_exact_registry_lookup();
    test_invalid_and_ambiguous_registry();
    test_task_id_mapping();
    return 0;
}

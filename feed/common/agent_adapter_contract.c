#include "agent_adapter_contract.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool bounded_component(const char *text, size_t capacity)
{
    size_t index;
    size_t length;

    if (text == NULL || capacity < 2U) {
        return false;
    }
    length = strnlen(text, capacity);
    if (length == 0U || length >= capacity) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)text[index];

        if (!(isalnum(character) || character == '.' || character == '_' ||
              character == '-' || character == ':' || character == '/' ||
              character == '@')) {
            return false;
        }
    }
    return true;
}

static bool valid_intent(const char *intent)
{
    size_t index;
    size_t length;

    if (intent == NULL) {
        return false;
    }
    length = strnlen(intent, AGENT_ADAPTER_INTENT_LEN);
    if (length == 0U || length >= AGENT_ADAPTER_INTENT_LEN ||
        intent[0] == '.' || intent[length - 1U] == '.') {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)intent[index];

        if (!(isalnum(character) || character == '.' || character == '_' ||
              character == '-')) {
            return false;
        }
        if (character == '.' && index > 0U && intent[index - 1U] == '.') {
            return false;
        }
    }
    return true;
}

enum agent_adapter_protocol agent_adapter_protocol_parse(const char *text)
{
    if (text == NULL) {
        return AGENT_ADAPTER_PROTOCOL_INVALID;
    }
    if (strcmp(text, "mcp") == 0) {
        return AGENT_ADAPTER_PROTOCOL_MCP;
    }
    if (strcmp(text, "a2a") == 0) {
        return AGENT_ADAPTER_PROTOCOL_A2A;
    }
    return AGENT_ADAPTER_PROTOCOL_INVALID;
}

const char *agent_adapter_protocol_name(enum agent_adapter_protocol protocol)
{
    switch (protocol) {
    case AGENT_ADAPTER_PROTOCOL_MCP:
        return "mcp";
    case AGENT_ADAPTER_PROTOCOL_A2A:
        return "a2a";
    default:
        return "invalid";
    }
}

bool agent_adapter_mapping_valid(const struct agent_adapter_mapping *mapping)
{
    if (mapping == NULL || !mapping->enabled ||
        (mapping->protocol != AGENT_ADAPTER_PROTOCOL_MCP &&
         mapping->protocol != AGENT_ADAPTER_PROTOCOL_A2A) ||
        mapping->intent_version == 0U ||
        (mapping->continuable && !mapping->task) ||
        mapping->recovery_protocol > 1U) {
        return false;
    }
    return bounded_component(mapping->authority,
                             AGENT_ADAPTER_AUTHORITY_LEN) &&
           bounded_component(mapping->selector,
                             AGENT_ADAPTER_SELECTOR_LEN) &&
           valid_intent(mapping->intent);
}

enum agent_adapter_result agent_adapter_registry_validate(
    const struct agent_adapter_mapping *mappings,
    size_t mapping_count
)
{
    size_t left;
    size_t right;

    if ((mappings == NULL && mapping_count != 0U) ||
        mapping_count > AGENT_ADAPTER_MAX_MAPPINGS) {
        return AGENT_ADAPTER_TOO_LARGE;
    }
    for (left = 0U; left < mapping_count; left++) {
        if (!agent_adapter_mapping_valid(&mappings[left])) {
            return AGENT_ADAPTER_INVALID;
        }
        for (right = left + 1U; right < mapping_count; right++) {
            if (mappings[left].protocol == mappings[right].protocol &&
                strcmp(mappings[left].authority,
                       mappings[right].authority) == 0 &&
                strcmp(mappings[left].selector,
                       mappings[right].selector) == 0) {
                return AGENT_ADAPTER_AMBIGUOUS;
            }
        }
    }
    return AGENT_ADAPTER_OK;
}

enum agent_adapter_result agent_adapter_lookup(
    const struct agent_adapter_mapping *mappings,
    size_t mapping_count,
    enum agent_adapter_protocol protocol,
    const char *authority,
    const char *selector,
    const struct agent_adapter_mapping **match
)
{
    const struct agent_adapter_mapping *candidate = NULL;
    size_t index;

    if (match == NULL || (mappings == NULL && mapping_count != 0U) ||
        mapping_count > AGENT_ADAPTER_MAX_MAPPINGS ||
        (protocol != AGENT_ADAPTER_PROTOCOL_MCP &&
         protocol != AGENT_ADAPTER_PROTOCOL_A2A) ||
        !bounded_component(authority, AGENT_ADAPTER_AUTHORITY_LEN) ||
        !bounded_component(selector, AGENT_ADAPTER_SELECTOR_LEN)) {
        return AGENT_ADAPTER_INVALID;
    }
    *match = NULL;
    for (index = 0U; index < mapping_count; index++) {
        if (!mappings[index].enabled || mappings[index].protocol != protocol ||
            strcmp(mappings[index].authority, authority) != 0 ||
            strcmp(mappings[index].selector, selector) != 0) {
            continue;
        }
        if (!agent_adapter_mapping_valid(&mappings[index])) {
            return AGENT_ADAPTER_INVALID;
        }
        if (candidate != NULL) {
            return AGENT_ADAPTER_AMBIGUOUS;
        }
        candidate = &mappings[index];
    }
    if (candidate == NULL) {
        return AGENT_ADAPTER_NOT_FOUND;
    }
    *match = candidate;
    return AGENT_ADAPTER_OK;
}

bool agent_adapter_make_task_id(
    enum agent_adapter_protocol protocol,
    const char *external_id,
    char output[AGENT_ADAPTER_TASK_ID_LEN]
)
{
    const char *prefix;
    int written;

    if (output == NULL ||
        !bounded_component(external_id, AGENT_ADAPTER_TASK_ID_LEN)) {
        return false;
    }
    if (protocol == AGENT_ADAPTER_PROTOCOL_MCP) {
        prefix = "mcp:";
    } else if (protocol == AGENT_ADAPTER_PROTOCOL_A2A) {
        prefix = "a2a:";
    } else {
        return false;
    }
    written = snprintf(output, AGENT_ADAPTER_TASK_ID_LEN, "%s%s",
                       prefix, external_id);
    return written > 0 && (size_t)written < AGENT_ADAPTER_TASK_ID_LEN;
}

#ifndef NEXUS_AGENT_ADAPTER_CONTRACT_H
#define NEXUS_AGENT_ADAPTER_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_ADAPTER_AUTHORITY_LEN 96U
#define AGENT_ADAPTER_SELECTOR_LEN 96U
#define AGENT_ADAPTER_INTENT_LEN 128U
#define AGENT_ADAPTER_TASK_ID_LEN 128U
#define AGENT_ADAPTER_TITLE_LEN 128U
#define AGENT_ADAPTER_DESCRIPTION_LEN 512U
#define AGENT_ADAPTER_INPUT_SCHEMA_LEN 2048U
#define AGENT_ADAPTER_MAX_MAPPINGS 256U

enum agent_adapter_protocol {
    AGENT_ADAPTER_PROTOCOL_INVALID = 0,
    AGENT_ADAPTER_PROTOCOL_MCP,
    AGENT_ADAPTER_PROTOCOL_A2A
};

enum agent_adapter_result {
    AGENT_ADAPTER_OK = 0,
    AGENT_ADAPTER_INVALID,
    AGENT_ADAPTER_NOT_FOUND,
    AGENT_ADAPTER_AMBIGUOUS,
    AGENT_ADAPTER_TOO_LARGE
};

struct agent_adapter_mapping {
    bool enabled;
    enum agent_adapter_protocol protocol;
    char authority[AGENT_ADAPTER_AUTHORITY_LEN];
    char selector[AGENT_ADAPTER_SELECTOR_LEN];
    char intent[AGENT_ADAPTER_INTENT_LEN];
    uint32_t intent_version;
    char title[AGENT_ADAPTER_TITLE_LEN];
    char description[AGENT_ADAPTER_DESCRIPTION_LEN];
    char input_schema_json[AGENT_ADAPTER_INPUT_SCHEMA_LEN];
    bool task;
    bool resumable;
    bool continuable;
    uint8_t recovery_protocol;
    bool demo;
    bool chat;
    bool interactive;
};

enum agent_adapter_protocol agent_adapter_protocol_parse(const char *text);

const char *agent_adapter_protocol_name(enum agent_adapter_protocol protocol);

bool agent_adapter_mapping_valid(const struct agent_adapter_mapping *mapping);

enum agent_adapter_result agent_adapter_registry_validate(
    const struct agent_adapter_mapping *mappings,
    size_t mapping_count
);

enum agent_adapter_result agent_adapter_lookup(
    const struct agent_adapter_mapping *mappings,
    size_t mapping_count,
    enum agent_adapter_protocol protocol,
    const char *authority,
    const char *selector,
    const struct agent_adapter_mapping **match
);

bool agent_adapter_make_task_id(
    enum agent_adapter_protocol protocol,
    const char *external_id,
    char output[AGENT_ADAPTER_TASK_ID_LEN]
);

#endif

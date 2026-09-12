#ifndef NEXUS_AGENT_DYNAMIC_MANIFEST_H
#define NEXUS_AGENT_DYNAMIC_MANIFEST_H

#include "agent_adapter_contract.h"
#include "agent_ipc_protocol.h"
#include "route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_DYNAMIC_MANIFEST_MAX_TOOLS 256U
#define AGENT_DYNAMIC_MANIFEST_MAX_ORIGIN_TOOLS 64U
#define AGENT_DYNAMIC_MANIFEST_NAME_LEN 256U
#define AGENT_DYNAMIC_MANIFEST_DIGEST_LEN 65U

enum agent_dynamic_manifest_result {
    AGENT_DYNAMIC_MANIFEST_OK = 0,
    AGENT_DYNAMIC_MANIFEST_INVALID,
    AGENT_DYNAMIC_MANIFEST_FULL,
    AGENT_DYNAMIC_MANIFEST_ORIGIN_FULL,
    AGENT_DYNAMIC_MANIFEST_NAME_CONFLICT,
    AGENT_DYNAMIC_MANIFEST_TENANT_CONFLICT,
    AGENT_DYNAMIC_MANIFEST_COMPUTER_POLICY_CONFLICT,
    AGENT_DYNAMIC_MANIFEST_MOBILE_POLICY_CONFLICT,
    AGENT_DYNAMIC_MANIFEST_SELECTOR_CONFLICT
};

struct agent_dynamic_manifest_entry {
    char route_id[AGENT_ROUTE_ID_LEN];
    char tenant[AGENT_TENANT_LEN];
    char origin[AGENT_URI_LEN];
    char agent_name[AGENT_DYNAMIC_MANIFEST_NAME_LEN];
    char manifest_digest[AGENT_DYNAMIC_MANIFEST_DIGEST_LEN];
    bool publish;
    bool computer_present;
    uint8_t computer_requirement;
    uint16_t workspace_capabilities;
    bool mobile_present;
    uint8_t mobile_requirement;
    uint16_t mobile_capabilities;
    uint16_t mobile_scopes;
    struct agent_adapter_mapping mapping;
};

struct agent_dynamic_manifest_table {
    struct agent_dynamic_manifest_entry
        entries[AGENT_DYNAMIC_MANIFEST_MAX_TOOLS];
    size_t count;
    uint64_t generation;
};

typedef bool (*agent_dynamic_manifest_route_active)(
    const char *route_id,
    void *context
);

void agent_dynamic_manifest_init(struct agent_dynamic_manifest_table *table);

enum agent_dynamic_manifest_result agent_dynamic_manifest_validate_upsert(
    const struct agent_dynamic_manifest_table *table,
    const struct agent_dynamic_manifest_entry *entry
);

enum agent_dynamic_manifest_result agent_dynamic_manifest_upsert(
    struct agent_dynamic_manifest_table *table,
    const struct agent_dynamic_manifest_entry *entry
);

bool agent_dynamic_manifest_remove(
    struct agent_dynamic_manifest_table *table,
    const char *route_id
);

size_t agent_dynamic_manifest_prune(
    struct agent_dynamic_manifest_table *table,
    agent_dynamic_manifest_route_active active,
    void *context
);

const char *agent_dynamic_manifest_result_name(
    enum agent_dynamic_manifest_result result
);

#endif
